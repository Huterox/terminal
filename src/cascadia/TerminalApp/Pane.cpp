// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Pane.h"
#include "AppLogic.h"

#include <Mmsystem.h>

using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Core;
using namespace winrt::Windows::UI::Xaml::Media;
using namespace winrt::Microsoft::Terminal::Settings::Model;
using namespace winrt::Microsoft::Terminal::Control;
using namespace winrt::Microsoft::Terminal::TerminalConnection;
using namespace winrt::TerminalApp;
using namespace TerminalApp;

static const int PaneBorderSize = 2;
static const int CombinedPaneBorderSize = 2 * PaneBorderSize;

// WARNING: Don't do this! This won't work
//   Duration duration{ std::chrono::milliseconds{ 200 } };
// Instead, make a duration from a TimeSpan from the time in millis
//
// 200ms was chosen because it's quick enough that it doesn't break your
// flow, but not too quick to see
static const int AnimationDurationInMilliseconds = 200;
static const Duration AnimationDuration = DurationHelper::FromTimeSpan(winrt::Windows::Foundation::TimeSpan(std::chrono::milliseconds(AnimationDurationInMilliseconds)));

winrt::Windows::UI::Xaml::Media::SolidColorBrush Pane::s_focusedBorderBrush = { nullptr };
winrt::Windows::UI::Xaml::Media::SolidColorBrush Pane::s_unfocusedBorderBrush = { nullptr };

Pane::Pane(const Profile& profile, const TermControl& control, const bool lastFocused) :
    _control{ control },
    _lastActive{ lastFocused },
    _profile{ profile }
{
    _root.Children().Append(_border);
    _border.Child(_control);

    _connectionStateChangedToken = _control.ConnectionStateChanged({ this, &Pane::_ControlConnectionStateChangedHandler });
    _warningBellToken = _control.WarningBell({ this, &Pane::_ControlWarningBellHandler });

    // On the first Pane's creation, lookup resources we'll use to theme the
    // Pane, including the brushed to use for the focused/unfocused border
    // color.
    if (s_focusedBorderBrush == nullptr || s_unfocusedBorderBrush == nullptr)
    {
        _SetupResources();
    }

    // Use the unfocused border color as the pane background, so an actual color
    // appears behind panes as we animate them sliding in.
    _root.Background(s_unfocusedBorderBrush);

    // Register an event with the control to have it inform us when it gains focus.
    _gotFocusRevoker = _control.GotFocus(winrt::auto_revoke, { this, &Pane::_ControlGotFocusHandler });
    _lostFocusRevoker = _control.LostFocus(winrt::auto_revoke, { this, &Pane::_ControlLostFocusHandler });

    // When our border is tapped, make sure to transfer focus to our control.
    // LOAD-BEARING: This will NOT work if the border's BorderBrush is set to
    // Colors::Transparent! The border won't get Tapped events, and they'll fall
    // through to something else.
    _border.Tapped([this](auto&, auto& e) {
        _FocusFirstChild();
        e.Handled(true);
    });
}

// Method Description:
// - Extract the terminal settings from the current (leaf) pane's control
//   to be used to create an equivalent control
// Arguments:
// - <none>
// Return Value:
// - Arguments appropriate for a SplitPane or NewTab action
NewTerminalArgs Pane::GetTerminalArgsForPane() const
{
    // Leaves are the only things that have controls
    assert(_IsLeaf());

    NewTerminalArgs args{};
    auto controlSettings = _control.Settings().as<TerminalSettings>();

    args.Profile(controlSettings.ProfileName());
    args.StartingDirectory(controlSettings.StartingDirectory());
    args.TabTitle(controlSettings.StartingTitle());
    args.Commandline(controlSettings.Commandline());
    args.SuppressApplicationTitle(controlSettings.SuppressApplicationTitle());
    if (controlSettings.TabColor() || controlSettings.StartingTabColor())
    {
        til::color c;
        // StartingTabColor is prioritized over other colors
        if (const auto color = controlSettings.StartingTabColor())
        {
            c = til::color(color.Value());
        }
        else
        {
            c = til::color(controlSettings.TabColor().Value());
        }

        args.TabColor(winrt::Windows::Foundation::IReference<winrt::Windows::UI::Color>(c));
    }

    if (controlSettings.AppliedColorScheme())
    {
        auto name = controlSettings.AppliedColorScheme().Name();
        args.ColorScheme(name);
    }

    return args;
}

// Method Description:
// - Serializes the state of this tab as a series of commands that can be
//   executed to recreate it.
// - This will always result in the right-most child being the focus
//   after the commands finish executing.
// Arguments:
// - currentId: the id to use for the current/first pane
// - nextId: the id to use for a new pane if we split
// Return Value:
// - The state from building the startup actions, includes a vector of commands,
//   the original root pane, the id of the focused pane, and the number of panes
//   created.
Pane::BuildStartupState Pane::BuildStartupActions(uint32_t currentId, uint32_t nextId)
{
    // if we are a leaf then all there is to do is defer to the parent.
    if (_IsLeaf())
    {
        if (_lastActive)
        {
            return { {}, shared_from_this(), currentId, 0 };
        }

        return { {}, shared_from_this(), std::nullopt, 0 };
    }

    auto buildSplitPane = [&](auto newPane) {
        ActionAndArgs actionAndArgs;
        actionAndArgs.Action(ShortcutAction::SplitPane);
        const auto terminalArgs{ newPane->GetTerminalArgsForPane() };
        // When creating a pane the split size is the size of the new pane
        // and not position.
        SplitPaneArgs args{ SplitType::Manual, _splitState, 1. - _desiredSplitPosition, terminalArgs };
        actionAndArgs.Args(args);

        return actionAndArgs;
    };

    auto buildMoveFocus = [](auto direction) {
        MoveFocusArgs args{ direction };

        ActionAndArgs actionAndArgs{};
        actionAndArgs.Action(ShortcutAction::MoveFocus);
        actionAndArgs.Args(args);

        return actionAndArgs;
    };

    // Handle simple case of a single split (a minor optimization for clarity)
    // Here we just create the second child (by splitting) and return the first
    // child for the parent to deal with.
    if (_firstChild->_IsLeaf() && _secondChild->_IsLeaf())
    {
        auto actionAndArgs = buildSplitPane(_secondChild);
        std::optional<uint32_t> focusedPaneId = std::nullopt;
        if (_firstChild->_lastActive)
        {
            focusedPaneId = currentId;
        }
        else if (_secondChild->_lastActive)
        {
            focusedPaneId = nextId;
        }

        return { { actionAndArgs }, _firstChild, focusedPaneId, 1 };
    }

    // We now need to execute the commands for each side of the tree
    // We've done one split, so the first-most child will have currentId, and the
    // one after it will be incremented.
    auto firstState = _firstChild->BuildStartupActions(currentId, nextId + 1);
    // the next id for the second branch depends on how many splits were in the
    // first child.
    auto secondState = _secondChild->BuildStartupActions(nextId, nextId + firstState.panesCreated + 1);

    std::vector<ActionAndArgs> actions{};
    actions.reserve(firstState.args.size() + secondState.args.size() + 3);

    // first we make our split
    const auto newSplit = buildSplitPane(secondState.firstPane);
    actions.emplace_back(std::move(newSplit));

    if (firstState.args.size() > 0)
    {
        // Then move to the first child and execute any actions on the left branch
        // then move back
        actions.emplace_back(buildMoveFocus(FocusDirection::PreviousInOrder));
        actions.insert(actions.end(), std::make_move_iterator(std::begin(firstState.args)), std::make_move_iterator(std::end(firstState.args)));
        actions.emplace_back(buildMoveFocus(FocusDirection::NextInOrder));
    }

    // And if there are any commands to run on the right branch do so
    if (secondState.args.size() > 0)
    {
        actions.insert(actions.end(), std::make_move_iterator(secondState.args.begin()), std::make_move_iterator(secondState.args.end()));
    }

    // if the tree is well-formed then f1.has_value and f2.has_value are
    // mutually exclusive.
    const auto focusedPaneId = firstState.focusedPaneId.has_value() ? firstState.focusedPaneId : secondState.focusedPaneId;

    return { actions, firstState.firstPane, focusedPaneId, firstState.panesCreated + secondState.panesCreated + 1 };
}

// Method Description:
// - Update the size of this pane. Resizes each of our columns so they have the
//   same relative sizes, given the newSize.
// - Because we're just manually setting the row/column sizes in pixels, we have
//   to be told our new size, we can't just use our own OnSized event, because
//   that _won't fire when we get smaller_.
// Arguments:
// - newSize: the amount of space that this pane has to fill now.
// Return Value:
// - <none>
void Pane::ResizeContent(const Size& newSize)
{
    const auto width = newSize.Width;
    const auto height = newSize.Height;

    _CreateRowColDefinitions();

    if (_splitState == SplitState::Vertical)
    {
        const auto paneSizes = _CalcChildrenSizes(width);

        const Size firstSize{ paneSizes.first, height };
        const Size secondSize{ paneSizes.second, height };
        _firstChild->ResizeContent(firstSize);
        _secondChild->ResizeContent(secondSize);
    }
    else if (_splitState == SplitState::Horizontal)
    {
        const auto paneSizes = _CalcChildrenSizes(height);

        const Size firstSize{ width, paneSizes.first };
        const Size secondSize{ width, paneSizes.second };
        _firstChild->ResizeContent(firstSize);
        _secondChild->ResizeContent(secondSize);
    }
}

// Method Description:
// - Recalculates and reapplies sizes of all descendant panes.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::Relayout()
{
    ResizeContent(_root.ActualSize());
}

// Method Description:
// - Adjust our child percentages to increase the size of one of our children
//   and decrease the size of the other.
// - Adjusts the separation amount by 5%
// - Does nothing if the direction doesn't match our current split direction
// Arguments:
// - direction: the direction to move our separator. If it's down or right,
//   we'll be increasing the size of the first of our children. Else, we'll be
//   decreasing the size of our first child.
// Return Value:
// - false if we couldn't resize this pane in the given direction, else true.
bool Pane::_Resize(const ResizeDirection& direction)
{
    if (!DirectionMatchesSplit(direction, _splitState))
    {
        return false;
    }

    float amount = .05f;
    if (direction == ResizeDirection::Right || direction == ResizeDirection::Down)
    {
        amount = -amount;
    }

    // Make sure we're not making a pane explode here by resizing it to 0 characters.
    const bool changeWidth = _splitState == SplitState::Vertical;

    const Size actualSize{ gsl::narrow_cast<float>(_root.ActualWidth()),
                           gsl::narrow_cast<float>(_root.ActualHeight()) };
    // actualDimension is the size in DIPs of this pane in the direction we're
    // resizing.
    const auto actualDimension = changeWidth ? actualSize.Width : actualSize.Height;

    _desiredSplitPosition = _ClampSplitPosition(changeWidth, _desiredSplitPosition - amount, actualDimension);

    // Resize our columns to match the new percentages.
    ResizeContent(actualSize);

    return true;
}

// Method Description:
// - Moves the separator between panes, as to resize each child on either size
//   of the separator. Tries to move a separator in the given direction. The
//   separator moved is the separator that's closest depth-wise to the
//   currently focused pane, that's also in the correct direction to be moved.
//   If there isn't such a separator, then this method returns false, as we
//   couldn't handle the resize.
// Arguments:
// - direction: The direction to move the separator in.
// Return Value:
// - true if we or a child handled this resize request.
bool Pane::ResizePane(const ResizeDirection& direction)
{
    // If we're a leaf, do nothing. We can't possibly have a descendant with a
    // separator the correct direction.
    if (_IsLeaf())
    {
        return false;
    }

    // Check if either our first or second child is the currently focused leaf.
    // If it is, and the requested resize direction matches our separator, then
    // we're the pane that needs to adjust its separator.
    // If our separator is the wrong direction, then we can't handle it.
    const bool firstIsFocused = _firstChild->_IsLeaf() && _firstChild->_lastActive;
    const bool secondIsFocused = _secondChild->_IsLeaf() && _secondChild->_lastActive;
    if (firstIsFocused || secondIsFocused)
    {
        return _Resize(direction);
    }

    // If neither of our children were the focused leaf, then recurse into
    // our children and see if they can handle the resize.
    // For each child, if it has a focused descendant, try having that child
    // handle the resize.
    // If the child wasn't able to handle the resize, it's possible that
    // there were no descendants with a separator the correct direction. If
    // our separator _is_ the correct direction, then we should be the pane
    // to resize. Otherwise, just return false, as we couldn't handle it
    // either.
    if ((!_firstChild->_IsLeaf()) && _firstChild->_HasFocusedChild())
    {
        return _firstChild->ResizePane(direction) || _Resize(direction);
    }

    if ((!_secondChild->_IsLeaf()) && _secondChild->_HasFocusedChild())
    {
        return _secondChild->ResizePane(direction) || _Resize(direction);
    }

    return false;
}

// Method Description:
// - Attempt to navigate from the sourcePane according to direction.
//   - If the direction is NextInOrder or PreviousInOrder, the next or previous
//     leaf in the tree, respectively, will be returned.
//   - If the direction is {Up, Down, Left, Right} then the visually-adjacent
//     neighbor (if it exists) will be returned. If there are multiple options
//     then the first-most leaf will be selected.
// Arguments:
// - sourcePane: the pane to navigate from
// - direction: which direction to go in
// - mruPanes: the list of most recently used panes, in order
// Return Value:
// - The result of navigating from source according to direction, which may be
//   nullptr (i.e. no pane was found in that direction).
std::shared_ptr<Pane> Pane::NavigateDirection(const std::shared_ptr<Pane> sourcePane, const FocusDirection& direction, const std::vector<uint32_t>& mruPanes)
{
    // Can't navigate anywhere if we are a leaf
    if (_IsLeaf())
    {
        return nullptr;
    }

    if (direction == FocusDirection::None)
    {
        return nullptr;
    }

    // Previous movement relies on the last used panes
    if (direction == FocusDirection::Previous)
    {
        // If there is actually a previous pane.
        if (mruPanes.size() > 1)
        {
            // This could return nullptr if the id is not actually in the tree.
            return FindPane(mruPanes.at(1));
        }
        return nullptr;
    }

    // Check if we in-order traversal is requested
    if (direction == FocusDirection::NextInOrder)
    {
        return NextPane(sourcePane);
    }

    if (direction == FocusDirection::PreviousInOrder)
    {
        return PreviousPane(sourcePane);
    }

    if (direction == FocusDirection::First)
    {
        std::shared_ptr<Pane> firstPane = nullptr;
        WalkTree([&](auto p) {
            if (p->_IsLeaf())
            {
                firstPane = p;
                return true;
            }

            return false;
        });

        // Don't need to do any movement if we are the source and target pane.
        if (firstPane == sourcePane)
        {
            return nullptr;
        }
        return firstPane;
    }

    // We are left with directional traversal now
    // If the focus direction does not match the split direction, the source pane
    // and its neighbor must necessarily be contained within the same child.
    if (!DirectionMatchesSplit(direction, _splitState))
    {
        if (const auto p = _firstChild->NavigateDirection(sourcePane, direction, mruPanes))
        {
            return p;
        }
        return _secondChild->NavigateDirection(sourcePane, direction, mruPanes);
    }

    // Since the direction is the same as our split, it is possible that we must
    // move focus from from one child to another child.
    // We now must keep track of state while we recurse.
    // If we have it, get the size of this pane.
    const auto scaleX = _root.ActualWidth() > 0 ? gsl::narrow_cast<float>(_root.ActualWidth()) : 1.f;
    const auto scaleY = _root.ActualHeight() > 0 ? gsl::narrow_cast<float>(_root.ActualHeight()) : 1.f;
    const auto paneNeighborPair = _FindPaneAndNeighbor(sourcePane, direction, { 0, 0, scaleX, scaleY });

    if (paneNeighborPair.source && paneNeighborPair.neighbor)
    {
        return paneNeighborPair.neighbor;
    }

    return nullptr;
}

// Method Description:
// - Attempts to find the succeeding pane of the provided pane.
// - NB: If targetPane is not a leaf, then this will return one of its children.
// Arguments:
// - targetPane: The pane to search for.
// Return Value:
// - The next pane in tree order after the target pane (if found)
std::shared_ptr<Pane> Pane::NextPane(const std::shared_ptr<Pane> targetPane)
{
    // if we are a leaf pane there is no next pane.
    if (_IsLeaf())
    {
        return nullptr;
    }

    std::shared_ptr<Pane> firstLeaf = nullptr;
    std::shared_ptr<Pane> nextPane = nullptr;
    bool foundTarget = false;

    auto foundNext = WalkTree([&](auto pane) {
        // In case the target pane is the last pane in the tree, keep a reference
        // to the first leaf so we can wrap around.
        if (firstLeaf == nullptr && pane->_IsLeaf())
        {
            firstLeaf = pane;
        }

        // If we've found the target pane already, get the next leaf pane.
        if (foundTarget && pane->_IsLeaf())
        {
            nextPane = pane;
            return true;
        }

        // Test if we're the target pane so we know to return the next pane.
        if (pane == targetPane)
        {
            foundTarget = true;
        }

        return false;
    });

    // If we found the desired pane just return it
    if (foundNext)
    {
        return nextPane;
    }

    // If we found the target pane, but not the next pane it means we were the
    // last leaf in the tree.
    if (foundTarget)
    {
        return firstLeaf;
    }

    return nullptr;
}

// Method Description:
// - Attempts to find the preceding pane of the provided pane.
// Arguments:
// - targetPane: The pane to search for.
// Return Value:
// - The previous pane in tree order before the target pane (if found)
std::shared_ptr<Pane> Pane::PreviousPane(const std::shared_ptr<Pane> targetPane)
{
    // if we are a leaf pane there is no previous pane.
    if (_IsLeaf())
    {
        return nullptr;
    }

    std::shared_ptr<Pane> lastLeaf = nullptr;
    bool foundTarget = false;

    WalkTree([&](auto pane) {
        if (pane == targetPane)
        {
            foundTarget = true;
            // If we were not the first leaf, then return the previous leaf.
            // Otherwise keep walking the tree to get the last pane.
            if (lastLeaf != nullptr)
            {
                return true;
            }
        }

        if (pane->_IsLeaf())
        {
            lastLeaf = pane;
        }

        return false;
    });

    // If we found the target pane then lastLeaf will either be the preceding
    // pane or the last pane in the tree if targetPane is the first leaf.
    if (foundTarget)
    {
        return lastLeaf;
    }

    return nullptr;
}

// Method Description:
// - Attempts to find the parent pane of the provided pane.
// Arguments:
// - pane: The pane to search for.
// Return Value:
// - the parent of `pane` if pane is in this tree.
std::shared_ptr<Pane> Pane::_FindParentOfPane(const std::shared_ptr<Pane> pane)
{
    if (_IsLeaf())
    {
        return nullptr;
    }

    if (_firstChild == pane || _secondChild == pane)
    {
        return shared_from_this();
    }

    if (auto p = _firstChild->_FindParentOfPane(pane))
    {
        return p;
    }

    return _secondChild->_FindParentOfPane(pane);
}

// Method Description:
// - Attempts to swap the location of the two given panes in the tree.
//   Searches the tree starting at this pane to find the parent pane for each of
//   the arguments, and if both parents are found, replaces the appropriate
//   child in each.
// Arguments:
// - first: A pointer to the first pane to switch.
// - second: A pointer to the second pane to switch.
// Return Value:
// - true if a swap was performed.
bool Pane::SwapPanes(std::shared_ptr<Pane> first, std::shared_ptr<Pane> second)
{
    // If there is nothing to swap, just return.
    if (first == second || _IsLeaf())
    {
        return false;
    }

    std::unique_lock lock{ _createCloseLock };

    // Recurse through the tree to find the parent panes of each pane that is
    // being swapped.
    std::shared_ptr<Pane> firstParent = _FindParentOfPane(first);
    std::shared_ptr<Pane> secondParent = _FindParentOfPane(second);

    // We should have found either no elements, or both elements.
    // If we only found one parent then the pane SwapPane was called on did not
    // contain both panes as leaves, as could happen if the tree was modified
    // after the pointers were found but before we reached this function.
    if (firstParent && secondParent)
    {
        // Swap size/display information of the two panes.
        std::swap(first->_borders, second->_borders);

        // Replace the old child with new one, and revoke appropriate event
        // handlers.
        auto replaceChild = [](auto& parent, auto oldChild, auto newChild) {
            // Revoke the old handlers
            if (parent->_firstChild == oldChild)
            {
                parent->_firstChild->Closed(parent->_firstClosedToken);
                parent->_firstChild = newChild;
            }
            else if (parent->_secondChild == oldChild)
            {
                parent->_secondChild->Closed(parent->_secondClosedToken);
                parent->_secondChild = newChild;
            }
            // Clear now to ensure that we can add the child's grid to us later
            parent->_root.Children().Clear();
        };

        // Make sure that the right event handlers are set, and the children
        // are placed in the appropriate locations in the grid.
        auto updateParent = [](auto& parent) {
            parent->_SetupChildCloseHandlers();
            parent->_root.Children().Clear();
            parent->_root.Children().Append(parent->_firstChild->GetRootElement());
            parent->_root.Children().Append(parent->_secondChild->GetRootElement());
            // Make sure they have the correct borders, and also that they are
            // placed in the right location in the grid.
            // This mildly reproduces ApplySplitDefinitions, but is different in
            // that it does not want to utilize the parent's border to set child
            // borders.
            if (parent->_splitState == SplitState::Vertical)
            {
                Controls::Grid::SetColumn(parent->_firstChild->GetRootElement(), 0);
                Controls::Grid::SetColumn(parent->_secondChild->GetRootElement(), 1);
            }
            else if (parent->_splitState == SplitState::Horizontal)
            {
                Controls::Grid::SetRow(parent->_firstChild->GetRootElement(), 0);
                Controls::Grid::SetRow(parent->_secondChild->GetRootElement(), 1);
            }
            parent->_firstChild->_UpdateBorders();
            parent->_secondChild->_UpdateBorders();
        };

        // If the firstParent and secondParent are the same, then we are just
        // swapping the first child and second child of that parent.
        if (firstParent == secondParent)
        {
            firstParent->_firstChild->Closed(firstParent->_firstClosedToken);
            firstParent->_secondChild->Closed(firstParent->_secondClosedToken);
            std::swap(firstParent->_firstChild, firstParent->_secondChild);

            updateParent(firstParent);
        }
        else
        {
            // Replace both children before updating display to ensure
            // that the grid elements are not attached to multiple panes
            replaceChild(firstParent, first, second);
            replaceChild(secondParent, second, first);
            updateParent(firstParent);
            updateParent(secondParent);
        }

        // For now the first pane is always the focused pane, so re-focus to
        // make sure the cursor is still in the terminal since the root was moved.
        first->_FocusFirstChild();

        return true;
    }

    return false;
}

// Method Description:
// - Given two panes' offsets, test whether the `direction` side of first is adjacent to second.
// Arguments:
// - firstOffset: The offset for the reference pane
// - secondOffset: the offset to test adjacency with.
// - direction: The direction to search in from the reference pane.
// Return Value:
// - true if the two panes are adjacent.
bool Pane::_IsAdjacent(const PanePoint firstOffset,
                       const PanePoint secondOffset,
                       const FocusDirection& direction) const
{
    // Since float equality is tricky (arithmetic is non-associative, commutative),
    // test if the two numbers are within an epsilon distance of each other.
    auto floatEqual = [](float left, float right) {
        return abs(left - right) < 1e-4F;
    };

    auto getXMax = [](PanePoint offset) {
        return offset.x + offset.scaleX;
    };

    auto getYMax = [](PanePoint offset) {
        return offset.y + offset.scaleY;
    };

    // When checking containment in a range, the range is half-closed, i.e. [x, x+w).
    // If the direction is left test that the left side of the first element is
    // next to the right side of the second element, and that the top left
    // corner of the first element is within the second element's height
    if (direction == FocusDirection::Left)
    {
        const auto sharesBorders = floatEqual(firstOffset.x, getXMax(secondOffset));
        const auto withinHeight = (firstOffset.y >= secondOffset.y) && (firstOffset.y < getYMax(secondOffset));

        return sharesBorders && withinHeight;
    }
    // If the direction is right test that the right side of the first element is
    // next to the left side of the second element, and that the top left
    // corner of the first element is within the second element's height
    else if (direction == FocusDirection::Right)
    {
        const auto sharesBorders = floatEqual(getXMax(firstOffset), secondOffset.x);
        const auto withinHeight = (firstOffset.y >= secondOffset.y) && (firstOffset.y < getYMax(secondOffset));

        return sharesBorders && withinHeight;
    }
    // If the direction is up test that the top side of the first element is
    // next to the bottom side of the second element, and that the top left
    // corner of the first element is within the second element's width
    else if (direction == FocusDirection::Up)
    {
        const auto sharesBorders = floatEqual(firstOffset.y, getYMax(secondOffset));
        const auto withinWidth = (firstOffset.x >= secondOffset.x) && (firstOffset.x < getXMax(secondOffset));

        return sharesBorders && withinWidth;
    }
    // If the direction is down test that the bottom side of the first element is
    // next to the top side of the second element, and that the top left
    // corner of the first element is within the second element's width
    else if (direction == FocusDirection::Down)
    {
        const auto sharesBorders = floatEqual(getYMax(firstOffset), secondOffset.y);
        const auto withinWidth = (firstOffset.x >= secondOffset.x) && (firstOffset.x < getXMax(secondOffset));

        return sharesBorders && withinWidth;
    }
    return false;
}

// Method Description:
// - Gets the offsets for the two children of this parent pane
// - If real dimensions are not available, simulated ones based on the split size
//   will be used instead.
// Arguments:
// - parentOffset the location and scale information of this pane.
// Return Value:
// - the two location/scale points for the children panes.
std::pair<Pane::PanePoint, Pane::PanePoint> Pane::_GetOffsetsForPane(const PanePoint parentOffset) const
{
    assert(!_IsLeaf());
    auto firstOffset = parentOffset;
    auto secondOffset = parentOffset;

    // Make up fake dimensions using an exponential layout. This is useful
    // since we might need to navigate when there are panes not attached  to
    // the ui tree, such as initialization, command running, and zoom.
    // Basically create the tree layout on the fly by partitioning [0,1].
    // This could run into issues if the tree depth is >127 (or other
    // degenerate splits) as a float's mantissa only has so many bits of
    // precision.

    if (_splitState == SplitState::Horizontal)
    {
        secondOffset.y += (1 - _desiredSplitPosition) * parentOffset.scaleY;
        firstOffset.scaleY *= _desiredSplitPosition;
        secondOffset.scaleY *= (1 - _desiredSplitPosition);
    }
    else
    {
        secondOffset.x += (1 - _desiredSplitPosition) * parentOffset.scaleX;
        firstOffset.scaleX *= _desiredSplitPosition;
        secondOffset.scaleX *= (1 - _desiredSplitPosition);
    }

    return { firstOffset, secondOffset };
}

// Method Description:
// - Given the source pane, and its relative position in the tree, attempt to
//   find its visual neighbor within the current pane's tree.
//   The neighbor, if it exists, will be a leaf pane.
// Arguments:
// - direction: The direction to search in from the source pane.
// - searchResult: the source pane and its relative position.
// - sourceIsSecondSide: If the source pane is on the "second" side (down/right of split)
//   relative to the branch being searched
// - offset: the offset of the current pane
// Return Value:
// - A tuple of Panes, the first being the focused pane if found, and the second
//   being the adjacent pane if it exists, and a bool that represents if the move
//   goes out of bounds.
Pane::PaneNeighborSearch Pane::_FindNeighborForPane(const FocusDirection& direction,
                                                    PaneNeighborSearch searchResult,
                                                    const bool sourceIsSecondSide,
                                                    const Pane::PanePoint offset)
{
    // Test if the move will go out of boundaries. E.g. if the focus is already
    // on the second child of some pane and it attempts to move right, there
    // can't possibly be a neighbor to be found in the first child.
    if ((sourceIsSecondSide && (direction == FocusDirection::Right || direction == FocusDirection::Down)) ||
        (!sourceIsSecondSide && (direction == FocusDirection::Left || direction == FocusDirection::Up)))
    {
        return searchResult;
    }

    // If we are a leaf node test if we adjacent to the focus node
    if (_IsLeaf())
    {
        if (_IsAdjacent(searchResult.sourceOffset, offset, direction))
        {
            searchResult.neighbor = shared_from_this();
        }
        return searchResult;
    }

    auto [firstOffset, secondOffset] = _GetOffsetsForPane(offset);
    auto sourceNeighborSearch = _firstChild->_FindNeighborForPane(direction, searchResult, sourceIsSecondSide, firstOffset);
    if (sourceNeighborSearch.neighbor)
    {
        return sourceNeighborSearch;
    }

    return _secondChild->_FindNeighborForPane(direction, searchResult, sourceIsSecondSide, secondOffset);
}

// Method Description:
// - Searches the tree to find the source pane, and if it exists, the
//   visually adjacent pane by direction.
// Arguments:
// - sourcePane: The pane to find the neighbor of.
// - direction: The direction to search in from the focused pane.
// - offset: The offset, with the top-left corner being (0,0), that the current pane is relative to the root.
// Return Value:
// - The (partial) search result. If the search was successful, the pane and its neighbor will be returned.
//   Otherwise, the neighbor will be null and the focus will be null/non-null if it was found.
Pane::PaneNeighborSearch Pane::_FindPaneAndNeighbor(const std::shared_ptr<Pane> sourcePane, const FocusDirection& direction, const Pane::PanePoint offset)
{
    // If we are the source pane, return ourselves
    if (this == sourcePane.get())
    {
        return { shared_from_this(), nullptr, offset };
    }

    if (_IsLeaf())
    {
        return { nullptr, nullptr, offset };
    }

    auto [firstOffset, secondOffset] = _GetOffsetsForPane(offset);

    auto sourceNeighborSearch = _firstChild->_FindPaneAndNeighbor(sourcePane, direction, firstOffset);
    // If we have both the focus element and its neighbor, we are done
    if (sourceNeighborSearch.source && sourceNeighborSearch.neighbor)
    {
        return sourceNeighborSearch;
    }
    // if we only found the focus, then we search the second branch for the
    // neighbor.
    if (sourceNeighborSearch.source)
    {
        // If we can possibly have both sides of a direction, check if the sibling has the neighbor
        if (DirectionMatchesSplit(direction, _splitState))
        {
            return _secondChild->_FindNeighborForPane(direction, sourceNeighborSearch, false, secondOffset);
        }
        return sourceNeighborSearch;
    }

    // If we didn't find the focus at all, we need to search the second branch
    // for the focus (and possibly its neighbor).
    sourceNeighborSearch = _secondChild->_FindPaneAndNeighbor(sourcePane, direction, secondOffset);
    // We found both so we are done.
    if (sourceNeighborSearch.source && sourceNeighborSearch.neighbor)
    {
        return sourceNeighborSearch;
    }
    // We only found the focus, which means that its neighbor might be in the
    // first branch.
    if (sourceNeighborSearch.source)
    {
        // If we can possibly have both sides of a direction, check if the sibling has the neighbor
        if (DirectionMatchesSplit(direction, _splitState))
        {
            return _firstChild->_FindNeighborForPane(direction, sourceNeighborSearch, true, firstOffset);
        }
        return sourceNeighborSearch;
    }

    return { nullptr, nullptr, offset };
}

// Method Description:
// - Called when our attached control is closed. Triggers listeners to our close
//   event, if we're a leaf pane.
// - If this was called, and we became a parent pane (due to work on another
//   thread), this function will do nothing (allowing the control's new parent
//   to handle the event instead).
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::_ControlConnectionStateChangedHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                                 const winrt::Windows::Foundation::IInspectable& /*args*/)
{
    std::unique_lock lock{ _createCloseLock };
    // It's possible that this event handler started being executed, then before
    // we got the lock, another thread created another child. So our control is
    // actually no longer _our_ control, and instead could be a descendant.
    //
    // When the control's new Pane takes ownership of the control, the new
    // parent will register it's own event handler. That event handler will get
    // fired after this handler returns, and will properly cleanup state.
    if (!_IsLeaf())
    {
        return;
    }

    const auto newConnectionState = _control.ConnectionState();
    const auto previousConnectionState = std::exchange(_connectionState, newConnectionState);

    if (newConnectionState < ConnectionState::Closed)
    {
        // Pane doesn't care if the connection isn't entering a terminal state.
        return;
    }

    if (previousConnectionState < ConnectionState::Connected && newConnectionState >= ConnectionState::Failed)
    {
        // A failure to complete the connection (before it has _connected_) is not covered by "closeOnExit".
        // This is to prevent a misconfiguration (closeOnExit: always, startingDirectory: garbage) resulting
        // in Terminal flashing open and immediately closed.
        return;
    }

    if (_profile)
    {
        const auto mode = _profile.CloseOnExit();
        if ((mode == CloseOnExitMode::Always) ||
            (mode == CloseOnExitMode::Graceful && newConnectionState == ConnectionState::Closed))
        {
            Close();
        }
    }
}

// Method Description:
// - Plays a warning note when triggered by the BEL control character,
//   using the sound configured for the "Critical Stop" system event.`
//   This matches the behavior of the Windows Console host.
// - Will also flash the taskbar if the bellStyle setting for this profile
//   has the 'visual' flag set
// Arguments:
// - <unused>
void Pane::_ControlWarningBellHandler(const winrt::Windows::Foundation::IInspectable& /*sender*/,
                                      const winrt::Windows::Foundation::IInspectable& /*eventArgs*/)
{
    if (!_IsLeaf())
    {
        return;
    }
    if (_profile)
    {
        // We don't want to do anything if nothing is set, so check for that first
        if (static_cast<int>(_profile.BellStyle()) != 0)
        {
            if (WI_IsFlagSet(_profile.BellStyle(), winrt::Microsoft::Terminal::Settings::Model::BellStyle::Audible))
            {
                // Audible is set, play the sound
                const auto soundAlias = reinterpret_cast<LPCTSTR>(SND_ALIAS_SYSTEMHAND);
                PlaySound(soundAlias, NULL, SND_ALIAS_ID | SND_ASYNC | SND_SENTRY);
            }

            if (WI_IsFlagSet(_profile.BellStyle(), winrt::Microsoft::Terminal::Settings::Model::BellStyle::Window))
            {
                _control.BellLightOn();
            }

            // raise the event with the bool value corresponding to the taskbar flag
            _PaneRaiseBellHandlers(nullptr, WI_IsFlagSet(_profile.BellStyle(), winrt::Microsoft::Terminal::Settings::Model::BellStyle::Taskbar));
        }
    }
}

// Event Description:
// - Called when our control gains focus. We'll use this to trigger our GotFocus
//   callback. The tab that's hosting us should have registered a callback which
//   can be used to mark us as active.
// Arguments:
// - <unused>
// Return Value:
// - <none>
void Pane::_ControlGotFocusHandler(winrt::Windows::Foundation::IInspectable const& /* sender */,
                                   RoutedEventArgs const& /* args */)
{
    _GotFocusHandlers(shared_from_this());
}

// Event Description:
// - Called when our control loses focus. We'll use this to trigger our LostFocus
//   callback. The tab that's hosting us should have registered a callback which
//   can be used to update its own internal focus state
void Pane::_ControlLostFocusHandler(winrt::Windows::Foundation::IInspectable const& /* sender */,
                                    RoutedEventArgs const& /* args */)
{
    _LostFocusHandlers(shared_from_this());
}

// Method Description:
// - Fire our Closed event to tell our parent that we should be removed.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::Close()
{
    // Fire our Closed event to tell our parent that we should be removed.
    _ClosedHandlers(nullptr, nullptr);
}

// Method Description:
// - Prepare this pane to be removed from the UI hierarchy by closing all controls
//   and connections beneath it.
void Pane::Shutdown()
{
    // Lock the create/close lock so that another operation won't concurrently
    // modify our tree
    std::unique_lock lock{ _createCloseLock };
    if (_IsLeaf())
    {
        _control.Close();
    }
    else
    {
        _firstChild->Shutdown();
        _secondChild->Shutdown();
    }
}

// Method Description:
// - Get the root UIElement of this pane. There may be a single TermControl as a
//   child, or an entire tree of grids and panes as children of this element.
// Arguments:
// - <none>
// Return Value:
// - the Grid acting as the root of this pane.
Controls::Grid Pane::GetRootElement()
{
    return _root;
}

// Method Description:
// - If this is the last focused pane, returns itself. Returns nullptr if this
//   is a leaf and it's not focused. If it's a parent, it returns nullptr if no
//   children of this pane were the last pane to be focused, or the Pane that
//   _was_ the last pane to be focused (if there was one).
// - This Pane's control might not currently be focused, if the tab itself is
//   not currently focused.
// Return Value:
// - nullptr if we're a leaf and unfocused, or no children were marked
//   `_lastActive`, else returns this
std::shared_ptr<Pane> Pane::GetActivePane()
{
    if (_IsLeaf())
    {
        return _lastActive ? shared_from_this() : nullptr;
    }

    auto firstFocused = _firstChild->GetActivePane();
    if (firstFocused != nullptr)
    {
        return firstFocused;
    }
    return _secondChild->GetActivePane();
}

// Method Description:
// - Gets the TermControl of this pane. If this Pane is not a leaf, this will return nullptr.
// Arguments:
// - <none>
// Return Value:
// - nullptr if this Pane is a parent, otherwise the TermControl of this Pane.
TermControl Pane::GetTerminalControl()
{
    return _IsLeaf() ? _control : nullptr;
}

// Method Description:
// - Recursively remove the "Active" state from this Pane and all it's children.
// - Updates our visuals to match our new state, including highlighting our borders.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::ClearActive()
{
    _lastActive = false;
    if (!_IsLeaf())
    {
        _firstChild->ClearActive();
        _secondChild->ClearActive();
    }
    UpdateVisuals();
}

// Method Description:
// - Sets the "Active" state on this Pane. Only one Pane in a tree of Panes
//   should be "active", and that pane should be a leaf.
// - Updates our visuals to match our new state, including highlighting our borders.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::SetActive()
{
    _lastActive = true;
    UpdateVisuals();
}

// Method Description:
// - Returns nullptr if no children of this pane were the last control to be
//   focused, or the profile of the last control to be focused (if there was
//   one).
// Arguments:
// - <none>
// Return Value:
// - nullptr if no children of this pane were the last control to be
//   focused, else the profile of the last control to be focused
Profile Pane::GetFocusedProfile()
{
    auto lastFocused = GetActivePane();
    return lastFocused ? lastFocused->_profile : nullptr;
}

// Method Description:
// - Returns true if this pane was the last pane to be focused in a tree of panes.
// Arguments:
// - <none>
// Return Value:
// - true iff we were the last pane focused in this tree of panes.
bool Pane::WasLastFocused() const noexcept
{
    return _lastActive;
}

// Method Description:
// - Returns true iff this pane has no child panes.
// Arguments:
// - <none>
// Return Value:
// - true iff this pane has no child panes.
bool Pane::_IsLeaf() const noexcept
{
    return _splitState == SplitState::None;
}

// Method Description:
// - Returns true if this pane is currently focused, or there is a pane which is
//   a child of this pane that is actively focused
// Arguments:
// - <none>
// Return Value:
// - true if the currently focused pane is either this pane, or one of this
//   pane's descendants
bool Pane::_HasFocusedChild() const noexcept
{
    // We're intentionally making this one giant expression, so the compiler
    // will skip the following lookups if one of the lookups before it returns
    // true
    return (_control && _lastActive) ||
           (_firstChild && _firstChild->_HasFocusedChild()) ||
           (_secondChild && _secondChild->_HasFocusedChild());
}

// Method Description:
// - Update the focus state of this pane. We'll make sure to colorize our
//   borders depending on if we are the active pane or not.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::UpdateVisuals()
{
    _border.BorderBrush(_lastActive ? s_focusedBorderBrush : s_unfocusedBorderBrush);
}

// Method Description:
// - Focuses this control if we're a leaf, or attempts to focus the first leaf
//   of our first child, recursively.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::_FocusFirstChild()
{
    if (_IsLeaf())
    {
        // Originally, we would only raise a GotFocus event here when:
        //
        // if (_root.ActualWidth() == 0 && _root.ActualHeight() == 0)
        //
        // When these sizes are 0, then the pane might still be in startup,
        // and doesn't yet have a real size. In that case, the control.Focus
        // event won't be handled until _after_ the startup events are all
        // processed. This will lead to the Tab not being notified that the
        // focus moved to a different Pane.
        //
        // However, with the ability to execute multiple actions at a time, in
        // already existing windows, we need to always raise this event manually
        // here, to correctly inform the Tab that we're now focused. This will
        // take care of commandlines like:
        //
        // `wtd -w 0 mf down ; sp`
        // `wtd -w 0 fp -t 1 ; sp`

        _GotFocusHandlers(shared_from_this());

        _control.Focus(FocusState::Programmatic);
    }
    else
    {
        _firstChild->_FocusFirstChild();
    }
}

// Method Description:
// - Updates the settings of this pane, presuming that it is a leaf.
// Arguments:
// - settings: The new TerminalSettings to apply to any matching controls
// - profile: The profile from which these settings originated.
// Return Value:
// - <none>
void Pane::UpdateSettings(const TerminalSettingsCreateResult& settings, const Profile& profile)
{
    assert(_IsLeaf());

    _profile = profile;
    auto controlSettings = _control.Settings().as<TerminalSettings>();
    // Update the parent of the control's settings object (and not the object itself) so
    // that any overrides made by the control don't get affected by the reload
    controlSettings.SetParent(settings.DefaultSettings());
    auto unfocusedSettings{ settings.UnfocusedSettings() };
    if (unfocusedSettings)
    {
        // Note: the unfocused settings needs to be entirely unchanged _except_ we need to
        // set its parent to the settings object that lives in the control. This is because
        // the overrides made by the control live in that settings object, so we want to make
        // sure the unfocused settings inherit from that.
        unfocusedSettings.SetParent(controlSettings);
    }
    _control.UnfocusedAppearance(unfocusedSettings);
    _control.UpdateSettings();
}

// Method Description:
// - Attempts to add the provided pane as a split of the current pane.
// Arguments:
// - pane: the new pane to add
// - splitType: How the pane should be attached
// Return Value:
// - the new reference to the child created from the current pane.
std::shared_ptr<Pane> Pane::AttachPane(std::shared_ptr<Pane> pane, SplitState splitType)
{
    // Splice the new pane into the tree
    const auto [first, _] = _Split(splitType, .5, pane);

    // If the new pane has a child that was the focus, re-focus it
    // to steal focus from the currently focused pane.
    if (pane->_HasFocusedChild())
    {
        pane->WalkTree([](auto p) {
            if (p->_lastActive)
            {
                p->_FocusFirstChild();
                return true;
            }
            return false;
        });
    }

    return first;
}

// Method Description:
// - Attempts to find the parent of the target pane,
//   if found remove the pane from the tree and return it.
// - If the removed pane was (or contained the focus) the first sibling will
//   gain focus.
// Arguments:
// - pane: the pane to detach
// Return Value:
// - The removed pane, if found.
std::shared_ptr<Pane> Pane::DetachPane(std::shared_ptr<Pane> pane)
{
    // We can't remove a pane if we only have a reference to a leaf, even if we
    // are the pane.
    if (_IsLeaf())
    {
        return nullptr;
    }

    // Check if either of our children matches the search
    const auto isFirstChild = _firstChild == pane;
    const auto isSecondChild = _secondChild == pane;

    if (isFirstChild || isSecondChild)
    {
        // Keep a reference to the child we are removing
        auto detached = isFirstChild ? _firstChild : _secondChild;
        // Remove the child from the tree, replace the current node with the
        // other child.
        _CloseChild(isFirstChild, true);

        detached->_borders = Borders::None;
        detached->_UpdateBorders();

        // Trigger the detached event on each child
        detached->WalkTree([](auto pane) {
            pane->_PaneDetachedHandlers(pane);
            return false;
        });

        return detached;
    }

    if (const auto detached = _firstChild->DetachPane(pane))
    {
        return detached;
    }

    return _secondChild->DetachPane(pane);
}

// Method Description:
// - Closes one of our children. In doing so, takes the control from the other
//   child, and makes this pane a leaf node again.
// Arguments:
// - closeFirst: if true, the first child should be closed, and the second
//   should be preserved, and vice-versa for false.
// - isDetaching: if true, then the pane event handlers for the closed child
//   should be kept, this way they don't have to be recreated when it is later
//   reattached to a tree somewhere as the control moves with the pane.
// Return Value:
// - <none>
void Pane::_CloseChild(const bool closeFirst, const bool isDetaching)
{
    // Lock the create/close lock so that another operation won't concurrently
    // modify our tree
    std::unique_lock lock{ _createCloseLock };

    // If we're a leaf, then chances are both our children closed in close
    // succession. We waited on the lock while the other child was closed, so
    // now we don't have a child to close anymore. Return here. When we moved
    // the non-closed child into us, we also set up event handlers that will be
    // triggered when we return from this.
    if (_IsLeaf())
    {
        return;
    }

    auto closedChild = closeFirst ? _firstChild : _secondChild;
    auto remainingChild = closeFirst ? _secondChild : _firstChild;
    auto closedChildClosedToken = closeFirst ? _firstClosedToken : _secondClosedToken;
    auto remainingChildClosedToken = closeFirst ? _secondClosedToken : _firstClosedToken;

    // If the only child left is a leaf, that means we're a leaf now.
    if (remainingChild->_IsLeaf())
    {
        // When the remaining child is a leaf, that means both our children were
        // previously leaves, and the only difference in their borders is the
        // border that we gave them. Take a bitwise AND of those two children to
        // remove that border. Other borders the children might have, they
        // inherited from us, so the flag will be set for both children.
        _borders = _firstChild->_borders & _secondChild->_borders;

        // take the control, profile and id of the pane that _wasn't_ closed.
        _control = remainingChild->_control;
        _connectionState = remainingChild->_connectionState;
        _profile = remainingChild->_profile;
        _id = remainingChild->Id();

        // Add our new event handler before revoking the old one.
        _connectionStateChangedToken = _control.ConnectionStateChanged({ this, &Pane::_ControlConnectionStateChangedHandler });
        _warningBellToken = _control.WarningBell({ this, &Pane::_ControlWarningBellHandler });

        // Revoke the old event handlers. Remove both the handlers for the panes
        // themselves closing, and remove their handlers for their controls
        // closing. At this point, if the remaining child's control is closed,
        // they'll trigger only our event handler for the control's close.

        // However, if we are detaching the pane we want to keep its control
        // handlers since it is just getting moved.
        if (!isDetaching)
        {
            closedChild->_control.ConnectionStateChanged(closedChild->_connectionStateChangedToken);
            closedChild->_control.WarningBell(closedChild->_warningBellToken);
        }

        closedChild->Closed(closedChildClosedToken);
        remainingChild->Closed(remainingChildClosedToken);
        remainingChild->_control.ConnectionStateChanged(remainingChild->_connectionStateChangedToken);
        remainingChild->_control.WarningBell(remainingChild->_warningBellToken);

        // If either of our children was focused, we want to take that focus from
        // them.
        _lastActive = _firstChild->_lastActive || _secondChild->_lastActive;

        // Remove all the ui elements of the remaining child. This'll make sure
        // we can re-attach the TermControl to our Grid.
        remainingChild->_root.Children().Clear();
        remainingChild->_border.Child(nullptr);

        // Reset our UI:
        _root.Children().Clear();
        _border.Child(nullptr);
        _root.ColumnDefinitions().Clear();
        _root.RowDefinitions().Clear();

        // Reattach the TermControl to our grid.
        _root.Children().Append(_border);
        _border.Child(_control);

        // Make sure to set our _splitState before focusing the control. If you
        // fail to do this, when the tab handles the GotFocus event and asks us
        // what our active control is, we won't technically be a "leaf", and
        // GetTerminalControl will return null.
        _splitState = SplitState::None;

        // re-attach our handler for the control's GotFocus event.
        _gotFocusRevoker = _control.GotFocus(winrt::auto_revoke, { this, &Pane::_ControlGotFocusHandler });
        _lostFocusRevoker = _control.LostFocus(winrt::auto_revoke, { this, &Pane::_ControlLostFocusHandler });

        // If we're inheriting the "last active" state from one of our children,
        // focus our control now. This should trigger our own GotFocus event.
        if (_lastActive)
        {
            _control.Focus(FocusState::Programmatic);

            // See GH#7252
            // Manually fire off the GotFocus event. Typically, this is done
            // automatically when the control gets focused. However, if we're
            // `exit`ing a zoomed pane, then the other sibling isn't in the UI
            // tree currently. So the above call to Focus won't actually focus
            // the control. Because Tab is relying on GotFocus to know who the
            // active pane in the tree is, without this call, _no one_ will be
            // the active pane any longer.
            _GotFocusHandlers(shared_from_this());
        }

        _UpdateBorders();

        // Release our children.
        _firstChild = nullptr;
        _secondChild = nullptr;
    }
    else
    {
        // Find what borders need to persist after we close the child
        auto remainingBorders = _GetCommonBorders();

        // Steal all the state from our child
        _splitState = remainingChild->_splitState;
        _firstChild = remainingChild->_firstChild;
        _secondChild = remainingChild->_secondChild;

        // Set up new close handlers on the children
        _SetupChildCloseHandlers();

        // Revoke the old event handlers on our new children
        _firstChild->Closed(remainingChild->_firstClosedToken);
        _secondChild->Closed(remainingChild->_secondClosedToken);

        // Remove the event handlers on the old children
        remainingChild->Closed(remainingChildClosedToken);
        closedChild->Closed(closedChildClosedToken);
        if (!isDetaching)
        {
            closedChild->_control.ConnectionStateChanged(closedChild->_connectionStateChangedToken);
            closedChild->_control.WarningBell(closedChild->_warningBellToken);
        }

        // Reset our UI:
        _root.Children().Clear();
        _border.Child(nullptr);
        _root.ColumnDefinitions().Clear();
        _root.RowDefinitions().Clear();

        // Copy the old UI over to our grid.
        // Start by copying the row/column definitions. Iterate over the
        // rows/cols, and remove each one from the old grid, and attach it to
        // our grid instead.
        while (remainingChild->_root.ColumnDefinitions().Size() > 0)
        {
            auto col = remainingChild->_root.ColumnDefinitions().GetAt(0);
            remainingChild->_root.ColumnDefinitions().RemoveAt(0);
            _root.ColumnDefinitions().Append(col);
        }
        while (remainingChild->_root.RowDefinitions().Size() > 0)
        {
            auto row = remainingChild->_root.RowDefinitions().GetAt(0);
            remainingChild->_root.RowDefinitions().RemoveAt(0);
            _root.RowDefinitions().Append(row);
        }

        // Remove the child's UI elements from the child's grid, so we can
        // attach them to us instead.
        remainingChild->_root.Children().Clear();
        remainingChild->_border.Child(nullptr);

        _root.Children().Append(_firstChild->GetRootElement());
        _root.Children().Append(_secondChild->GetRootElement());

        // Propagate the new borders down to the children.
        _borders = remainingBorders;
        _ApplySplitDefinitions();

        // If the closed child was focused, transfer the focus to it's first sibling.
        if (closedChild->_lastActive)
        {
            _FocusFirstChild();
        }

        // Release the pointers that the child was holding.
        remainingChild->_firstChild = nullptr;
        remainingChild->_secondChild = nullptr;
    }
}

winrt::fire_and_forget Pane::_CloseChildRoutine(const bool closeFirst)
{
    auto weakThis{ shared_from_this() };

    co_await winrt::resume_foreground(_root.Dispatcher());

    if (auto pane{ weakThis.get() })
    {
        // This will query if animations are enabled via the "Show animations in
        // Windows" setting in the OS
        winrt::Windows::UI::ViewManagement::UISettings uiSettings;
        const auto animationsEnabledInOS = uiSettings.AnimationsEnabled();
        const auto animationsEnabledInApp = Media::Animation::Timeline::AllowDependentAnimations();

        // GH#7252: If either child is zoomed, just skip the animation. It won't work.
        const bool eitherChildZoomed = pane->_firstChild->_zoomed || pane->_secondChild->_zoomed;
        // If animations are disabled, just skip this and go straight to
        // _CloseChild. Curiously, the pane opening animation doesn't need this,
        // and will skip straight to Completed when animations are disabled, but
        // this one doesn't seem to.
        if (!animationsEnabledInOS || !animationsEnabledInApp || eitherChildZoomed)
        {
            pane->_CloseChild(closeFirst, false);
            co_return;
        }

        // Setup the animation

        auto removedChild = closeFirst ? _firstChild : _secondChild;
        auto remainingChild = closeFirst ? _secondChild : _firstChild;
        const bool splitWidth = _splitState == SplitState::Vertical;

        Size removedOriginalSize{
            ::base::saturated_cast<float>(removedChild->_root.ActualWidth()),
            ::base::saturated_cast<float>(removedChild->_root.ActualHeight())
        };
        Size remainingOriginalSize{
            ::base::saturated_cast<float>(remainingChild->_root.ActualWidth()),
            ::base::saturated_cast<float>(remainingChild->_root.ActualHeight())
        };

        // Remove both children from the grid
        _root.Children().Clear();
        // Add the remaining child back to the grid, in the right place.
        _root.Children().Append(remainingChild->GetRootElement());
        if (_splitState == SplitState::Vertical)
        {
            Controls::Grid::SetColumn(remainingChild->GetRootElement(), closeFirst ? 1 : 0);
        }
        else if (_splitState == SplitState::Horizontal)
        {
            Controls::Grid::SetRow(remainingChild->GetRootElement(), closeFirst ? 1 : 0);
        }

        // Create the dummy grid. This grid will be the one we actually animate,
        // in the place of the closed pane.
        Controls::Grid dummyGrid;
        dummyGrid.Background(s_unfocusedBorderBrush);
        // It should be the size of the closed pane.
        dummyGrid.Width(removedOriginalSize.Width);
        dummyGrid.Height(removedOriginalSize.Height);
        // Put it where the removed child is
        if (_splitState == SplitState::Vertical)
        {
            Controls::Grid::SetColumn(dummyGrid, closeFirst ? 0 : 1);
        }
        else if (_splitState == SplitState::Horizontal)
        {
            Controls::Grid::SetRow(dummyGrid, closeFirst ? 0 : 1);
        }
        // Add it to the tree
        _root.Children().Append(dummyGrid);

        // Set up the rows/cols as auto/auto, so they'll only use the size of
        // the elements in the grid.
        //
        // * For the closed pane, we want to make that row/col "auto" sized, so
        //   it takes up as much space as is available.
        // * For the remaining pane, we'll make that row/col "*" sized, so it
        //   takes all the remaining space. As the dummy grid is resized down,
        //   the remaining pane will expand to take the rest of the space.
        _root.ColumnDefinitions().Clear();
        _root.RowDefinitions().Clear();
        if (_splitState == SplitState::Vertical)
        {
            auto firstColDef = Controls::ColumnDefinition();
            auto secondColDef = Controls::ColumnDefinition();
            firstColDef.Width(!closeFirst ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
            secondColDef.Width(closeFirst ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
            _root.ColumnDefinitions().Append(firstColDef);
            _root.ColumnDefinitions().Append(secondColDef);
        }
        else if (_splitState == SplitState::Horizontal)
        {
            auto firstRowDef = Controls::RowDefinition();
            auto secondRowDef = Controls::RowDefinition();
            firstRowDef.Height(!closeFirst ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
            secondRowDef.Height(closeFirst ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
            _root.RowDefinitions().Append(firstRowDef);
            _root.RowDefinitions().Append(secondRowDef);
        }

        // Animate the dummy grid from its current size down to 0
        Media::Animation::DoubleAnimation animation{};
        animation.Duration(AnimationDuration);
        animation.From(splitWidth ? removedOriginalSize.Width : removedOriginalSize.Height);
        animation.To(0.0);
        // This easing is the same as the entrance animation.
        animation.EasingFunction(Media::Animation::QuadraticEase{});
        animation.EnableDependentAnimation(true);

        Media::Animation::Storyboard s;
        s.Duration(AnimationDuration);
        s.Children().Append(animation);
        s.SetTarget(animation, dummyGrid);
        s.SetTargetProperty(animation, splitWidth ? L"Width" : L"Height");

        // Start the animation.
        s.Begin();

        std::weak_ptr<Pane> weakThis{ shared_from_this() };

        // When the animation is completed, reparent the child's content up to
        // us, and remove the child nodes from the tree.
        animation.Completed([weakThis, closeFirst](auto&&, auto&&) {
            if (auto pane{ weakThis.lock() })
            {
                // We don't need to manually undo any of the above trickiness.
                // We're going to re-parent the child's content into us anyways
                pane->_CloseChild(closeFirst, false);
            }
        });
    }
}

// Method Description:
// - Adds event handlers to our children to handle their close events.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::_SetupChildCloseHandlers()
{
    _firstClosedToken = _firstChild->Closed([this](auto&& /*s*/, auto&& /*e*/) {
        _CloseChildRoutine(true);
    });

    _secondClosedToken = _secondChild->Closed([this](auto&& /*s*/, auto&& /*e*/) {
        _CloseChildRoutine(false);
    });
}

// Method Description:
// - Sets up row/column definitions for this pane. There are three total
//   row/cols. The middle one is for the separator. The first and third are for
//   each of the child panes, and are given a size in pixels, based off the
//   available space, and the percent of the space they respectively consume,
//   which is stored in _desiredSplitPosition
// - Does nothing if our split state is currently set to SplitState::None
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::_CreateRowColDefinitions()
{
    const auto first = _desiredSplitPosition * 100.0f;
    const auto second = 100.0f - first;
    if (_splitState == SplitState::Vertical)
    {
        _root.ColumnDefinitions().Clear();

        // Create two columns in this grid: one for each pane

        auto firstColDef = Controls::ColumnDefinition();
        firstColDef.Width(GridLengthHelper::FromValueAndType(first, GridUnitType::Star));

        auto secondColDef = Controls::ColumnDefinition();
        secondColDef.Width(GridLengthHelper::FromValueAndType(second, GridUnitType::Star));

        _root.ColumnDefinitions().Append(firstColDef);
        _root.ColumnDefinitions().Append(secondColDef);
    }
    else if (_splitState == SplitState::Horizontal)
    {
        _root.RowDefinitions().Clear();

        // Create two rows in this grid: one for each pane

        auto firstRowDef = Controls::RowDefinition();
        firstRowDef.Height(GridLengthHelper::FromValueAndType(first, GridUnitType::Star));

        auto secondRowDef = Controls::RowDefinition();
        secondRowDef.Height(GridLengthHelper::FromValueAndType(second, GridUnitType::Star));

        _root.RowDefinitions().Append(firstRowDef);
        _root.RowDefinitions().Append(secondRowDef);
    }
}

// Method Description:
// - Sets the thickness of each side of our borders to match our _borders state.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::_UpdateBorders()
{
    double top = 0, bottom = 0, left = 0, right = 0;

    Thickness newBorders{ 0 };
    if (_zoomed)
    {
        // When the pane is zoomed, manually show all the borders around the window.
        top = bottom = right = left = PaneBorderSize;
    }
    else
    {
        if (WI_IsFlagSet(_borders, Borders::Top))
        {
            top = PaneBorderSize;
        }
        if (WI_IsFlagSet(_borders, Borders::Bottom))
        {
            bottom = PaneBorderSize;
        }
        if (WI_IsFlagSet(_borders, Borders::Left))
        {
            left = PaneBorderSize;
        }
        if (WI_IsFlagSet(_borders, Borders::Right))
        {
            right = PaneBorderSize;
        }
    }
    _border.BorderThickness(ThicknessHelper::FromLengths(left, top, right, bottom));
}

// Method Description:
// - Find the borders for the leaf pane, or the shared borders for child panes.
// Arguments:
// - <none>
// Return Value:
// - <none>
Borders Pane::_GetCommonBorders()
{
    if (_IsLeaf())
    {
        return _borders;
    }

    return _firstChild->_GetCommonBorders() & _secondChild->_GetCommonBorders();
}

// Method Description:
// - Sets the row/column of our child UI elements, to match our current split type.
// - In case the split definition or parent borders were changed, this recursively
//   updates the children as well.
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::_ApplySplitDefinitions()
{
    if (_splitState == SplitState::Vertical)
    {
        Controls::Grid::SetColumn(_firstChild->GetRootElement(), 0);
        Controls::Grid::SetColumn(_secondChild->GetRootElement(), 1);

        _firstChild->_borders = _borders | Borders::Right;
        _secondChild->_borders = _borders | Borders::Left;
        _borders = Borders::None;

        _firstChild->_ApplySplitDefinitions();
        _secondChild->_ApplySplitDefinitions();
    }
    else if (_splitState == SplitState::Horizontal)
    {
        Controls::Grid::SetRow(_firstChild->GetRootElement(), 0);
        Controls::Grid::SetRow(_secondChild->GetRootElement(), 1);

        _firstChild->_borders = _borders | Borders::Bottom;
        _secondChild->_borders = _borders | Borders::Top;
        _borders = Borders::None;

        _firstChild->_ApplySplitDefinitions();
        _secondChild->_ApplySplitDefinitions();
    }
    _UpdateBorders();
}

// Method Description:
// - Create a pair of animations when a new control enters this pane. This
//   should _ONLY_ be called in _Split, AFTER the first and second child panes
//   have been set up.
void Pane::_SetupEntranceAnimation()
{
    // This will query if animations are enabled via the "Show animations in
    // Windows" setting in the OS
    winrt::Windows::UI::ViewManagement::UISettings uiSettings;
    const auto animationsEnabledInOS = uiSettings.AnimationsEnabled();
    const auto animationsEnabledInApp = Media::Animation::Timeline::AllowDependentAnimations();

    const bool splitWidth = _splitState == SplitState::Vertical;
    const auto totalSize = splitWidth ? _root.ActualWidth() : _root.ActualHeight();
    // If we don't have a size yet, it's likely that we're in startup, or we're
    // being executed as a sequence of actions. In that case, just skip the
    // animation.
    if (totalSize <= 0 || !animationsEnabledInOS || !animationsEnabledInApp)
    {
        return;
    }

    const auto [firstSize, secondSize] = _CalcChildrenSizes(::base::saturated_cast<float>(totalSize));

    // This is safe to capture this, because it's only being called in the
    // context of this method (not on another thread)
    auto setupAnimation = [&](const auto& size, const bool isFirstChild) {
        auto child = isFirstChild ? _firstChild : _secondChild;
        auto childGrid = child->_root;
        auto control = child->_control;
        // Build up our animation:
        // * it'll take as long as our duration (200ms)
        // * it'll change the value of our property from 0 to secondSize
        // * it'll animate that value using a quadratic function (like f(t) = t^2)
        // * IMPORTANT! We'll manually tell the animation that "yes we know what
        //   we're doing, we want an animation here."
        Media::Animation::DoubleAnimation animation{};
        animation.Duration(AnimationDuration);
        if (isFirstChild)
        {
            // If we're animating the first pane, the size should decrease, from
            // the full size down to the given size.
            animation.From(totalSize);
            animation.To(size);
        }
        else
        {
            // Otherwise, we want to show the pane getting larger, so animate
            // from 0 to the requested size.
            animation.From(0.0);
            animation.To(size);
        }
        animation.EasingFunction(Media::Animation::QuadraticEase{});
        animation.EnableDependentAnimation(true);

        // Now we're going to set up the Storyboard. This is a unit that uses the
        // Animation from above, and actually applies it to a property.
        // * we'll set it up for the same duration as the animation we have
        // * Apply the animation to the grid of the new pane we're adding to the tree.
        // * apply the animation to the Width or Height property.
        Media::Animation::Storyboard s;
        s.Duration(AnimationDuration);
        s.Children().Append(animation);
        s.SetTarget(animation, childGrid);
        s.SetTargetProperty(animation, splitWidth ? L"Width" : L"Height");

        // BE TRICKY:
        // We're animating the width or height of our child pane's grid.
        //
        // We DON'T want to change the size of the control itself, because the
        // terminal has to reflow the buffer every time the control changes size. So
        // what we're going to do there is manually set the control's size to how
        // big we _actually know_ the control will be.
        //
        // We're also going to be changing alignment of our child pane and the
        // control. This way, we'll be able to have the control stick to the inside
        // of the child pane's grid (the side that's moving), while we also have the
        // pane's grid stick to "outside" of the grid (the side that's not moving)
        if (splitWidth)
        {
            // If we're animating the first child, then stick to the top/left of
            // the parent pane, otherwise use the bottom/right. This is always
            // the "outside" of the parent pane.
            childGrid.HorizontalAlignment(isFirstChild ? HorizontalAlignment::Left : HorizontalAlignment::Right);
            control.HorizontalAlignment(HorizontalAlignment::Left);
            control.Width(isFirstChild ? totalSize : size);

            // When the animation is completed, undo the trickiness from before, to
            // restore the controls to the behavior they'd usually have.
            animation.Completed([childGrid, control](auto&&, auto&&) {
                control.Width(NAN);
                childGrid.Width(NAN);
                childGrid.HorizontalAlignment(HorizontalAlignment::Stretch);
                control.HorizontalAlignment(HorizontalAlignment::Stretch);
            });
        }
        else
        {
            // If we're animating the first child, then stick to the top/left of
            // the parent pane, otherwise use the bottom/right. This is always
            // the "outside" of the parent pane.
            childGrid.VerticalAlignment(isFirstChild ? VerticalAlignment::Top : VerticalAlignment::Bottom);
            control.VerticalAlignment(VerticalAlignment::Top);
            control.Height(isFirstChild ? totalSize : size);

            // When the animation is completed, undo the trickiness from before, to
            // restore the controls to the behavior they'd usually have.
            animation.Completed([childGrid, control](auto&&, auto&&) {
                control.Height(NAN);
                childGrid.Height(NAN);
                childGrid.VerticalAlignment(VerticalAlignment::Stretch);
                control.VerticalAlignment(VerticalAlignment::Stretch);
            });
        }

        // Start the animation.
        s.Begin();
    };

    // TODO: GH#7365 - animating the first child right now doesn't _really_ do
    // anything. We could do better though.
    setupAnimation(firstSize, true);
    setupAnimation(secondSize, false);
}

// Method Description:
// - This is a helper to determine if a given Pane can be split, but without
//   using the ActualWidth() and ActualHeight() methods. This is used during
//   processing of many "split-pane" commands, which could happen _before_ we've
//   laid out a Pane for the first time. When this happens, the Pane's don't
//   have an actual size yet. However, we'd still like to figure out if the pane
//   could be split, once they're all laid out.
// - This method assumes that the Pane we're attempting to split is `target`,
//   and this method should be called on the root of a tree of Panes.
// - We'll walk down the tree attempting to find `target`. As we traverse the
//   tree, we'll reduce the size passed to each subsequent recursive call. The
//   size passed to this method represents how much space this Pane _will_ have
//   to use.
//   * If this pane is a leaf, and it's the pane we're looking for, use the
//     available space to calculate which direction to split in.
//   * If this pane is _any other leaf_, then just return nullopt, to indicate
//     that the `target` Pane is not down this branch.
//   * If this pane is a parent, calculate how much space our children will be
//     able to use, and recurse into them.
// Arguments:
// - target: The Pane we're attempting to split.
// - splitType: The direction we're attempting to split in.
// - availableSpace: The theoretical space that's available for this pane to be able to split.
// Return Value:
// - nullopt if `target` is not this pane or a child of this pane, otherwise
//   true iff we could split this pane, given `availableSpace`
// Note:
// - This method is highly similar to Pane::PreCalculateAutoSplit
std::optional<bool> Pane::PreCalculateCanSplit(const std::shared_ptr<Pane> target,
                                               SplitState splitType,
                                               const float splitSize,
                                               const winrt::Windows::Foundation::Size availableSpace) const
{
    if (_IsLeaf())
    {
        if (target.get() == this)
        {
            const auto firstPrecent = 1.0f - splitSize;
            const auto secondPercent = splitSize;
            // If this pane is a leaf, and it's the pane we're looking for, use
            // the available space to calculate which direction to split in.
            const Size minSize = _GetMinSize();

            if (splitType == SplitState::None)
            {
                return { false };
            }

            else if (splitType == SplitState::Vertical)
            {
                const auto widthMinusSeparator = availableSpace.Width - CombinedPaneBorderSize;
                const auto newFirstWidth = widthMinusSeparator * firstPrecent;
                const auto newSecondWidth = widthMinusSeparator * secondPercent;

                return { newFirstWidth > minSize.Width && newSecondWidth > minSize.Width };
            }

            else if (splitType == SplitState::Horizontal)
            {
                const auto heightMinusSeparator = availableSpace.Height - CombinedPaneBorderSize;
                const auto newFirstHeight = heightMinusSeparator * firstPrecent;
                const auto newSecondHeight = heightMinusSeparator * secondPercent;

                return { newFirstHeight > minSize.Height && newSecondHeight > minSize.Height };
            }
        }
        else
        {
            // If this pane is _any other leaf_, then just return nullopt, to
            // indicate that the `target` Pane is not down this branch.
            return std::nullopt;
        }
    }
    else
    {
        // If this pane is a parent, calculate how much space our children will
        // be able to use, and recurse into them.

        const bool isVerticalSplit = _splitState == SplitState::Vertical;
        const float firstWidth = isVerticalSplit ?
                                     (availableSpace.Width * _desiredSplitPosition) - PaneBorderSize :
                                     availableSpace.Width;
        const float secondWidth = isVerticalSplit ?
                                      (availableSpace.Width - firstWidth) - PaneBorderSize :
                                      availableSpace.Width;
        const float firstHeight = !isVerticalSplit ?
                                      (availableSpace.Height * _desiredSplitPosition) - PaneBorderSize :
                                      availableSpace.Height;
        const float secondHeight = !isVerticalSplit ?
                                       (availableSpace.Height - firstHeight) - PaneBorderSize :
                                       availableSpace.Height;

        const auto firstResult = _firstChild->PreCalculateCanSplit(target, splitType, splitSize, { firstWidth, firstHeight });
        return firstResult.has_value() ? firstResult : _secondChild->PreCalculateCanSplit(target, splitType, splitSize, { secondWidth, secondHeight });
    }

    // We should not possibly be getting here - both the above branches should
    // return a value.
    FAIL_FAST();
}

// Method Description:
// - Split the focused pane in our tree of panes, and place the given
//   TermControl into the newly created pane. If we're the focused pane, then
//   we'll create two new children, and place them side-by-side in our Grid.
// Arguments:
// - splitType: what type of split we want to create.
// - profile: The profile to associate with the newly created pane.
// - control: A TermControl to use in the new pane.
// Return Value:
// - The two newly created Panes
std::pair<std::shared_ptr<Pane>, std::shared_ptr<Pane>> Pane::Split(SplitState splitType,
                                                                    const float splitSize,
                                                                    const Profile& profile,
                                                                    const TermControl& control)
{
    if (!_IsLeaf())
    {
        if (_firstChild->_HasFocusedChild())
        {
            return _firstChild->Split(splitType, splitSize, profile, control);
        }
        else if (_secondChild->_HasFocusedChild())
        {
            return _secondChild->Split(splitType, splitSize, profile, control);
        }

        return { nullptr, nullptr };
    }

    auto newPane = std::make_shared<Pane>(profile, control);
    return _Split(splitType, splitSize, newPane);
}

// Method Description:
// - Toggle the split orientation of the currently focused pane
// Arguments:
// - <none>
// Return Value:
// - true if a split was changed
bool Pane::ToggleSplitOrientation()
{
    // If we are a leaf there is no split to toggle.
    if (_IsLeaf())
    {
        return false;
    }

    // Check if either our first or second child is the currently focused leaf.
    // If they are then switch the split orientation on the current pane.
    const bool firstIsFocused = _firstChild->_IsLeaf() && _firstChild->_lastActive;
    const bool secondIsFocused = _secondChild->_IsLeaf() && _secondChild->_lastActive;
    if (firstIsFocused || secondIsFocused)
    {
        // Switch the split orientation
        _splitState = _splitState == SplitState::Horizontal ? SplitState::Vertical : SplitState::Horizontal;

        // then update the borders and positioning on ourselves and our children.
        _borders = _GetCommonBorders();
        // Since we changed if we are using rows/columns, make sure we remove the old definitions
        _root.ColumnDefinitions().Clear();
        _root.RowDefinitions().Clear();
        _CreateRowColDefinitions();
        _ApplySplitDefinitions();

        return true;
    }

    return _firstChild->ToggleSplitOrientation() || _secondChild->ToggleSplitOrientation();
}

// Method Description:
// - Converts an "automatic" split type into either Vertical or Horizontal,
//   based upon the current dimensions of the Pane.
// - If any of the other SplitState values are passed in, they're returned
//   unmodified.
// Arguments:
// - splitType: The SplitState to attempt to convert
// Return Value:
// - None if splitType was None, otherwise one of Horizontal or Vertical
SplitState Pane::_convertAutomaticSplitState(const SplitState& splitType) const
{
    // Careful here! If the pane doesn't yet have a size, these dimensions will
    // be 0, and we'll always return Vertical.

    if (splitType == SplitState::Automatic)
    {
        // If the requested split type was "auto", determine which direction to
        // split based on our current dimensions
        const Size actualSize{ gsl::narrow_cast<float>(_root.ActualWidth()),
                               gsl::narrow_cast<float>(_root.ActualHeight()) };
        return actualSize.Width >= actualSize.Height ? SplitState::Vertical : SplitState::Horizontal;
    }
    return splitType;
}

// Method Description:
// - Does the bulk of the work of creating a new split. Initializes our UI,
//   creates a new Pane to host the control, registers event handlers.
// Arguments:
// - splitType: what type of split we should create.
// - splitSize: what fraction of the pane the new pane should get
// - newPane: the pane to add as a child
// Return Value:
// - The two newly created Panes
std::pair<std::shared_ptr<Pane>, std::shared_ptr<Pane>> Pane::_Split(SplitState splitType,
                                                                     const float splitSize,
                                                                     std::shared_ptr<Pane> newPane)
{
    if (splitType == SplitState::None)
    {
        return { nullptr, nullptr };
    }

    auto actualSplitType = _convertAutomaticSplitState(splitType);

    // Lock the create/close lock so that another operation won't concurrently
    // modify our tree
    std::unique_lock lock{ _createCloseLock };

    // revoke our handler - the child will take care of the control now.
    _control.ConnectionStateChanged(_connectionStateChangedToken);
    _connectionStateChangedToken.value = 0;
    _control.WarningBell(_warningBellToken);
    _warningBellToken.value = 0;

    // Remove our old GotFocus handler from the control. We don't what the
    // control telling us that it's now focused, we want it telling its new
    // parent.
    _gotFocusRevoker.revoke();
    _lostFocusRevoker.revoke();

    _splitState = actualSplitType;
    _desiredSplitPosition = 1.0f - splitSize;

    // Remove any children we currently have. We can't add the existing
    // TermControl to a new grid until we do this.
    _root.Children().Clear();
    _border.Child(nullptr);

    // Create two new Panes
    //   Move our control, guid into the first one.
    //   Move the new guid, control into the second.
    _firstChild = std::make_shared<Pane>(_profile, _control);
    _firstChild->_connectionState = std::exchange(_connectionState, ConnectionState::NotConnected);
    _profile = nullptr;
    _control = { nullptr };
    _secondChild = newPane;

    _CreateRowColDefinitions();

    _root.Children().Append(_firstChild->GetRootElement());
    _root.Children().Append(_secondChild->GetRootElement());

    _ApplySplitDefinitions();

    // Register event handlers on our children to handle their Close events
    _SetupChildCloseHandlers();

    _lastActive = false;

    _SetupEntranceAnimation();

    // Clear out our ID, only leaves should have IDs
    _id = {};

    return { _firstChild, _secondChild };
}

// Method Description:
// - Recursively attempt to "zoom" the given pane. When the pane is zoomed, it
//   won't be displayed as part of the tab tree, instead it'll take up the full
//   content of the tab. When we find the given pane, we'll need to remove it
//   from the UI tree, so that the caller can re-add it. We'll also set some
//   internal state, so the pane can display all of its borders.
// Arguments:
// - zoomedPane: This is the pane which we're attempting to zoom on.
// Return Value:
// - <none>
void Pane::Maximize(std::shared_ptr<Pane> zoomedPane)
{
    if (_IsLeaf())
    {
        _zoomed = (zoomedPane == shared_from_this());
        _UpdateBorders();
    }
    else
    {
        if (zoomedPane == _firstChild || zoomedPane == _secondChild)
        {
            // When we're zooming the pane, we'll need to remove it from our UI
            // tree. Easy way: just remove both children. We'll re-attach both
            // when we un-zoom.
            _root.Children().Clear();
        }

        // Always recurse into both children. If the (un)zoomed pane was one of
        // our direct children, we'll still want to update it's borders.
        _firstChild->Maximize(zoomedPane);
        _secondChild->Maximize(zoomedPane);
    }
}

// Method Description:
// - Recursively attempt to "un-zoom" the given pane. This does the opposite of
//   Pane::Maximize. When we find the given pane, we should return the pane to our
//   UI tree. We'll also clear the internal state, so the pane can display its
//   borders correctly.
// - The caller should make sure to have removed the zoomed pane from the UI
//   tree _before_ calling this.
// Arguments:
// - zoomedPane: This is the pane which we're attempting to un-zoom.
// Return Value:
// - <none>
void Pane::Restore(std::shared_ptr<Pane> zoomedPane)
{
    if (_IsLeaf())
    {
        _zoomed = false;
        _UpdateBorders();
    }
    else
    {
        if (zoomedPane == _firstChild || zoomedPane == _secondChild)
        {
            // When we're un-zooming the pane, we'll need to re-add it to our UI
            // tree where it originally belonged. easy way: just re-add both.
            _root.Children().Clear();
            _root.Children().Append(_firstChild->GetRootElement());
            _root.Children().Append(_secondChild->GetRootElement());
        }

        // Always recurse into both children. If the (un)zoomed pane was one of
        // our direct children, we'll still want to update it's borders.
        _firstChild->Restore(zoomedPane);
        _secondChild->Restore(zoomedPane);
    }
}

// Method Description:
// - Retrieves the ID of this pane
// - NOTE: The caller should make sure that this pane is a leaf,
//   otherwise the ID value will not make sense (leaves have IDs, parents do not)
// Return Value:
// - The ID of this pane
std::optional<uint32_t> Pane::Id() noexcept
{
    return _id;
}

// Method Description:
// - Sets this pane's ID
// - Panes are given IDs upon creation by TerminalTab
// Arguments:
// - The number to set this pane's ID to
void Pane::Id(uint32_t id) noexcept
{
    _id = id;
}

// Method Description:
// - Recursive function that focuses a pane with the given ID
// Arguments:
// - The ID of the pane we want to focus
bool Pane::FocusPane(const uint32_t id)
{
    if (_IsLeaf() && id == _id)
    {
        // Make sure to use _FocusFirstChild here - that'll properly update the
        // focus if we're in startup.
        _FocusFirstChild();
        return true;
    }
    else
    {
        if (_firstChild && _secondChild)
        {
            return _firstChild->FocusPane(id) ||
                   _secondChild->FocusPane(id);
        }
    }
    return false;
}
// Method Description:
// - Focuses the given pane if it is in the tree.
//   This deliberately mirrors FocusPane(id) instead of just calling
//   _FocusFirstChild directly.
// Arguments:
// - the pane to focus
// Return Value:
// - true if focus was set
bool Pane::FocusPane(const std::shared_ptr<Pane> pane)
{
    if (_IsLeaf() && this == pane.get())
    {
        // Make sure to use _FocusFirstChild here - that'll properly update the
        // focus if we're in startup.
        _FocusFirstChild();
        return true;
    }
    else
    {
        if (_firstChild && _secondChild)
        {
            return _firstChild->FocusPane(pane) ||
                   _secondChild->FocusPane(pane);
        }
    }
    return false;
}

// Method Description:
// - Recursive function that finds a pane with the given ID
// Arguments:
// - The ID of the pane we want to find
// Return Value:
// - A pointer to the pane with the given ID, if found.
std::shared_ptr<Pane> Pane::FindPane(const uint32_t id)
{
    if (_IsLeaf())
    {
        if (id == _id)
        {
            return shared_from_this();
        }
    }
    else
    {
        if (auto pane = _firstChild->FindPane(id))
        {
            return pane;
        }
        if (auto pane = _secondChild->FindPane(id))
        {
            return pane;
        }
    }

    return nullptr;
}

// Method Description:
// - Gets the size in pixels of each of our children, given the full size they
//   should fill. Since these children own their own separators (borders), this
//   size is their portion of our _entire_ size. If specified size is lower than
//   required then children will be of minimum size. Snaps first child to grid
//   but not the second.
// Arguments:
// - fullSize: the amount of space in pixels that should be filled by our
//   children and their separators. Can be arbitrarily low.
// Return Value:
// - a pair with the size of our first child and the size of our second child,
//   respectively.
std::pair<float, float> Pane::_CalcChildrenSizes(const float fullSize) const
{
    const auto widthOrHeight = _splitState == SplitState::Vertical;
    const auto snappedSizes = _CalcSnappedChildrenSizes(widthOrHeight, fullSize).lower;

    // Keep the first pane snapped and give the second pane all remaining size
    return {
        snappedSizes.first,
        fullSize - snappedSizes.first
    };
}

// Method Description:
// - Gets the size in pixels of each of our children, given the full size they should
//   fill. Each child is snapped to char grid as close as possible. If called multiple
//   times with fullSize argument growing, then both returned sizes are guaranteed to be
//   non-decreasing (it's a monotonically increasing function). This is important so that
//   user doesn't get any pane shrank when they actually expand the window or parent pane.
//   That is also required by the layout algorithm.
// Arguments:
// - widthOrHeight: if true, operates on width, otherwise on height.
// - fullSize: the amount of space in pixels that should be filled by our children and
//   their separator. Can be arbitrarily low.
// Return Value:
// - a structure holding the result of this calculation. The 'lower' field represents the
//   children sizes that would fit in the fullSize, but might (and usually do) not fill it
//   completely. The 'higher' field represents the size of the children if they slightly exceed
//   the fullSize, but are snapped. If the children can be snapped and also exactly match
//   the fullSize, then both this fields have the same value that represent this situation.
Pane::SnapChildrenSizeResult Pane::_CalcSnappedChildrenSizes(const bool widthOrHeight, const float fullSize) const
{
    if (_IsLeaf())
    {
        THROW_HR(E_FAIL);
    }

    //   First we build a tree of nodes corresponding to the tree of our descendant panes.
    // Each node represents a size of given pane. At the beginning, each node has the minimum
    // size that the corresponding pane can have; so has the our (root) node. We then gradually
    // expand our node (which in turn expands some of the child nodes) until we hit the desired
    // size. Since each expand step (done in _AdvanceSnappedDimension()) guarantees that all the
    // sizes will be snapped, our return values is also snapped.
    //   Why do we do it this, iterative way? Why can't we just split the given size by
    // _desiredSplitPosition and snap it latter? Because it's hardly doable, if possible, to also
    // fulfill the monotonicity requirement that way. As the fullSize increases, the proportional
    // point that separates children panes also moves and cells sneak in the available area in
    // unpredictable way, regardless which child has the snap priority or whether we snap them
    // upward, downward or to nearest.
    //   With present way we run the same sequence of actions regardless to the fullSize value and
    // only just stop at various moments when the built sizes reaches it.  Eventually, this could
    // be optimized for simple cases like when both children are both leaves with the same character
    // size, but it doesn't seem to be beneficial.

    auto sizeTree = _CreateMinSizeTree(widthOrHeight);
    LayoutSizeNode lastSizeTree{ sizeTree };

    while (sizeTree.size < fullSize)
    {
        lastSizeTree = sizeTree;
        _AdvanceSnappedDimension(widthOrHeight, sizeTree);

        if (sizeTree.size == fullSize)
        {
            // If we just hit exactly the requested value, then just return the
            // current state of children.
            return { { sizeTree.firstChild->size, sizeTree.secondChild->size },
                     { sizeTree.firstChild->size, sizeTree.secondChild->size } };
        }
    }

    // We exceeded the requested size in the loop above, so lastSizeTree will have
    // the last good sizes (so that children fit in) and sizeTree has the next possible
    // snapped sizes. Return them as lower and higher snap possibilities.
    return { { lastSizeTree.firstChild->size, lastSizeTree.secondChild->size },
             { sizeTree.firstChild->size, sizeTree.secondChild->size } };
}

// Method Description:
// - Adjusts given dimension (width or height) so that all descendant terminals
//   align with their character grids as close as possible. Snaps to closes match
//   (either upward or downward). Also makes sure to fit in minimal sizes of the panes.
// Arguments:
// - widthOrHeight: if true operates on width, otherwise on height
// - dimension: a dimension (width or height) to snap
// Return Value:
// - A value corresponding to the next closest snap size for this Pane, either upward or downward
float Pane::CalcSnappedDimension(const bool widthOrHeight, const float dimension) const
{
    const auto [lower, higher] = _CalcSnappedDimension(widthOrHeight, dimension);
    return dimension - lower < higher - dimension ? lower : higher;
}

// Method Description:
// - Adjusts given dimension (width or height) so that all descendant terminals
//   align with their character grids as close as possible. Also makes sure to
//   fit in minimal sizes of the panes.
// Arguments:
// - widthOrHeight: if true operates on width, otherwise on height
// - dimension: a dimension (width or height) to be snapped
// Return Value:
// - pair of floats, where first value is the size snapped downward (not greater then
//   requested size) and second is the size snapped upward (not lower than requested size).
//   If requested size is already snapped, then both returned values equal this value.
Pane::SnapSizeResult Pane::_CalcSnappedDimension(const bool widthOrHeight, const float dimension) const
{
    if (_IsLeaf())
    {
        // If we're a leaf pane, align to the grid of controlling terminal

        const auto minSize = _GetMinSize();
        const auto minDimension = widthOrHeight ? minSize.Width : minSize.Height;

        if (dimension <= minDimension)
        {
            return { minDimension, minDimension };
        }

        float lower = _control.SnapDimensionToGrid(widthOrHeight, dimension);
        if (widthOrHeight)
        {
            lower += WI_IsFlagSet(_borders, Borders::Left) ? PaneBorderSize : 0;
            lower += WI_IsFlagSet(_borders, Borders::Right) ? PaneBorderSize : 0;
        }
        else
        {
            lower += WI_IsFlagSet(_borders, Borders::Top) ? PaneBorderSize : 0;
            lower += WI_IsFlagSet(_borders, Borders::Bottom) ? PaneBorderSize : 0;
        }

        if (lower == dimension)
        {
            // If we happen to be already snapped, then just return this size
            // as both lower and higher values.
            return { lower, lower };
        }
        else
        {
            const auto cellSize = _control.CharacterDimensions();
            const auto higher = lower + (widthOrHeight ? cellSize.Width : cellSize.Height);
            return { lower, higher };
        }
    }
    else if (_splitState == (widthOrHeight ? SplitState::Horizontal : SplitState::Vertical))
    {
        // If we're resizing along separator axis, snap to the closest possibility
        // given by our children panes.

        const auto firstSnapped = _firstChild->_CalcSnappedDimension(widthOrHeight, dimension);
        const auto secondSnapped = _secondChild->_CalcSnappedDimension(widthOrHeight, dimension);
        return {
            std::max(firstSnapped.lower, secondSnapped.lower),
            std::min(firstSnapped.higher, secondSnapped.higher)
        };
    }
    else
    {
        // If we're resizing perpendicularly to separator axis, calculate the sizes
        // of child panes that would fit the given size. We use same algorithm that
        // is used for real resize routine, but exclude the remaining empty space that
        // would appear after the second pane. This will be the 'downward' snap possibility,
        // while the 'upward' will be given as a side product of the layout function.

        const auto childSizes = _CalcSnappedChildrenSizes(widthOrHeight, dimension);
        return {
            childSizes.lower.first + childSizes.lower.second,
            childSizes.higher.first + childSizes.higher.second
        };
    }
}

// Method Description:
// - Increases size of given LayoutSizeNode to match next possible 'snap'. In case of leaf
//   pane this means the next cell of the terminal. Otherwise it means that one of its children
//   advances (recursively). It expects the given node and its descendants to have either
//   already snapped or minimum size.
// Arguments:
// - widthOrHeight: if true operates on width, otherwise on height.
// - sizeNode: a layout size node that corresponds to this pane.
// Return Value:
// - <none>
void Pane::_AdvanceSnappedDimension(const bool widthOrHeight, LayoutSizeNode& sizeNode) const
{
    if (_IsLeaf())
    {
        // We're a leaf pane, so just add one more row or column (unless isMinimumSize
        // is true, see below).

        if (sizeNode.isMinimumSize)
        {
            // If the node is of its minimum size, this size might not be snapped (it might
            // be, say, half a character, or fixed 10 pixels), so snap it upward. It might
            // however be already snapped, so add 1 to make sure it really increases
            // (not strictly necessary but to avoid surprises).
            sizeNode.size = _CalcSnappedDimension(widthOrHeight, sizeNode.size + 1).higher;
        }
        else
        {
            const auto cellSize = _control.CharacterDimensions();
            sizeNode.size += widthOrHeight ? cellSize.Width : cellSize.Height;
        }
    }
    else
    {
        // We're a parent pane, so we have to advance dimension of our children panes. In
        // fact, we advance only one child (chosen later) to keep the growth fine-grained.

        // To choose which child pane to advance, we actually need to know their advanced sizes
        // in advance (oh), to see which one would 'fit' better. Often, this is already cached
        // by the previous invocation of this function in nextFirstChild and nextSecondChild
        // fields of given node. If not, we need to calculate them now.
        if (sizeNode.nextFirstChild == nullptr)
        {
            sizeNode.nextFirstChild = std::make_unique<LayoutSizeNode>(*sizeNode.firstChild);
            _firstChild->_AdvanceSnappedDimension(widthOrHeight, *sizeNode.nextFirstChild);
        }
        if (sizeNode.nextSecondChild == nullptr)
        {
            sizeNode.nextSecondChild = std::make_unique<LayoutSizeNode>(*sizeNode.secondChild);
            _secondChild->_AdvanceSnappedDimension(widthOrHeight, *sizeNode.nextSecondChild);
        }

        const auto nextFirstSize = sizeNode.nextFirstChild->size;
        const auto nextSecondSize = sizeNode.nextSecondChild->size;

        // Choose which child pane to advance.
        bool advanceFirstOrSecond;
        if (_splitState == (widthOrHeight ? SplitState::Horizontal : SplitState::Vertical))
        {
            // If we're growing along separator axis, choose the child that
            // wants to be smaller than the other, so that the resulting size
            // will be the smallest.
            advanceFirstOrSecond = nextFirstSize < nextSecondSize;
        }
        else
        {
            // If we're growing perpendicularly to separator axis, choose a
            // child so that their size ratio is closer to that we're trying
            // to maintain (this is, the relative separator position is closer
            // to the _desiredSplitPosition field).

            const auto firstSize = sizeNode.firstChild->size;
            const auto secondSize = sizeNode.secondChild->size;

            // Because we rely on equality check, these calculations have to be
            // immune to floating point errors. In common situation where both panes
            // have the same character sizes and _desiredSplitPosition is 0.5 (or
            // some simple fraction) both ratios will often be the same, and if so
            // we always take the left child. It could be right as well, but it's
            // important that it's consistent: that it would always go
            // 1 -> 2 -> 1 -> 2 -> 1 -> 2 and not like 1 -> 1 -> 2 -> 2 -> 2 -> 1
            // which would look silly to the user but which occur if there was
            // a non-floating-point-safe math.
            const auto deviation1 = nextFirstSize - (nextFirstSize + secondSize) * _desiredSplitPosition;
            const auto deviation2 = -1 * (firstSize - (firstSize + nextSecondSize) * _desiredSplitPosition);
            advanceFirstOrSecond = deviation1 <= deviation2;
        }

        // Here we advance one of our children. Because we already know the appropriate
        // (advanced) size that given child would need to have, we simply assign that size
        // to it. We then advance its 'next*' size (nextFirstChild or nextSecondChild) so
        // the invariant holds (as it will likely be used by the next invocation of this
        // function). The other child's next* size remains unchanged because its size
        // haven't changed either.
        if (advanceFirstOrSecond)
        {
            *sizeNode.firstChild = *sizeNode.nextFirstChild;
            _firstChild->_AdvanceSnappedDimension(widthOrHeight, *sizeNode.nextFirstChild);
        }
        else
        {
            *sizeNode.secondChild = *sizeNode.nextSecondChild;
            _secondChild->_AdvanceSnappedDimension(widthOrHeight, *sizeNode.nextSecondChild);
        }

        // Since the size of one of our children has changed we need to update our size as well.
        if (_splitState == (widthOrHeight ? SplitState::Horizontal : SplitState::Vertical))
        {
            sizeNode.size = std::max(sizeNode.firstChild->size, sizeNode.secondChild->size);
        }
        else
        {
            sizeNode.size = sizeNode.firstChild->size + sizeNode.secondChild->size;
        }
    }

    // Because we have grown, we're certainly no longer of our
    // minimal size (if we've ever been).
    sizeNode.isMinimumSize = false;
}

// Method Description:
// - Get the absolute minimum size that this pane can be resized to and still
//   have 1x1 character visible, in each of its children. If we're a leaf, we'll
//   include the space needed for borders _within_ us.
// Arguments:
// - <none>
// Return Value:
// - The minimum size that this pane can be resized to and still have a visible
//   character.
Size Pane::_GetMinSize() const
{
    if (_IsLeaf())
    {
        auto controlSize = _control.MinimumSize();
        auto newWidth = controlSize.Width;
        auto newHeight = controlSize.Height;

        newWidth += WI_IsFlagSet(_borders, Borders::Left) ? PaneBorderSize : 0;
        newWidth += WI_IsFlagSet(_borders, Borders::Right) ? PaneBorderSize : 0;
        newHeight += WI_IsFlagSet(_borders, Borders::Top) ? PaneBorderSize : 0;
        newHeight += WI_IsFlagSet(_borders, Borders::Bottom) ? PaneBorderSize : 0;

        return { newWidth, newHeight };
    }
    else
    {
        const auto firstSize = _firstChild->_GetMinSize();
        const auto secondSize = _secondChild->_GetMinSize();

        const auto minWidth = _splitState == SplitState::Vertical ?
                                  firstSize.Width + secondSize.Width :
                                  std::max(firstSize.Width, secondSize.Width);
        const auto minHeight = _splitState == SplitState::Horizontal ?
                                   firstSize.Height + secondSize.Height :
                                   std::max(firstSize.Height, secondSize.Height);

        return { minWidth, minHeight };
    }
}

// Method Description:
// - Builds a tree of LayoutSizeNode that matches the tree of panes. Each node
//   has minimum size that the corresponding pane can have.
// Arguments:
// - widthOrHeight: if true operates on width, otherwise on height
// Return Value:
// - Root node of built tree that matches this pane.
Pane::LayoutSizeNode Pane::_CreateMinSizeTree(const bool widthOrHeight) const
{
    const auto size = _GetMinSize();
    LayoutSizeNode node(widthOrHeight ? size.Width : size.Height);
    if (!_IsLeaf())
    {
        node.firstChild = std::make_unique<LayoutSizeNode>(_firstChild->_CreateMinSizeTree(widthOrHeight));
        node.secondChild = std::make_unique<LayoutSizeNode>(_secondChild->_CreateMinSizeTree(widthOrHeight));
    }

    return node;
}

// Method Description:
// - Adjusts split position so that no child pane is smaller then its
//   minimum size
// Arguments:
// - widthOrHeight: if true, operates on width, otherwise on height.
// - requestedValue: split position value to be clamped
// - totalSize: size (width or height) of the parent pane
// Return Value:
// - split position (value in range <0.0, 1.0>)
float Pane::_ClampSplitPosition(const bool widthOrHeight, const float requestedValue, const float totalSize) const
{
    const auto firstMinSize = _firstChild->_GetMinSize();
    const auto secondMinSize = _secondChild->_GetMinSize();

    const auto firstMinDimension = widthOrHeight ? firstMinSize.Width : firstMinSize.Height;
    const auto secondMinDimension = widthOrHeight ? secondMinSize.Width : secondMinSize.Height;

    const auto minSplitPosition = firstMinDimension / totalSize;
    const auto maxSplitPosition = 1.0f - (secondMinDimension / totalSize);

    return std::clamp(requestedValue, minSplitPosition, maxSplitPosition);
}

// Function Description:
// - Attempts to load some XAML resources that the Pane will need. This includes:
//   * The Color we'll use for active Panes's borders - SystemAccentColor
//   * The Brush we'll use for inactive Panes - TabViewBackground (to match the
//     color of the titlebar)
// Arguments:
// - <none>
// Return Value:
// - <none>
void Pane::_SetupResources()
{
    const auto res = Application::Current().Resources();
    const auto accentColorKey = winrt::box_value(L"SystemAccentColor");
    if (res.HasKey(accentColorKey))
    {
        const auto colorFromResources = res.Lookup(accentColorKey);
        // If SystemAccentColor is _not_ a Color for some reason, use
        // Transparent as the color, so we don't do this process again on
        // the next pane (by leaving s_focusedBorderBrush nullptr)
        auto actualColor = winrt::unbox_value_or<Color>(colorFromResources, Colors::Black());
        s_focusedBorderBrush = SolidColorBrush(actualColor);
    }
    else
    {
        // DON'T use Transparent here - if it's "Transparent", then it won't
        // be able to hittest for clicks, and then clicking on the border
        // will eat focus.
        s_focusedBorderBrush = SolidColorBrush{ Colors::Black() };
    }

    const auto unfocusedBorderBrushKey = winrt::box_value(L"UnfocusedBorderBrush");
    if (res.HasKey(unfocusedBorderBrushKey))
    {
        winrt::Windows::Foundation::IInspectable obj = res.Lookup(unfocusedBorderBrushKey);
        s_unfocusedBorderBrush = obj.try_as<winrt::Windows::UI::Xaml::Media::SolidColorBrush>();
    }
    else
    {
        // DON'T use Transparent here - if it's "Transparent", then it won't
        // be able to hittest for clicks, and then clicking on the border
        // will eat focus.
        s_unfocusedBorderBrush = SolidColorBrush{ Colors::Black() };
    }
}

int Pane::GetLeafPaneCount() const noexcept
{
    return _IsLeaf() ? 1 : (_firstChild->GetLeafPaneCount() + _secondChild->GetLeafPaneCount());
}

// Method Description:
// - This is a helper to determine which direction an "Automatic" split should
//   happen in for a given pane, but without using the ActualWidth() and
//   ActualHeight() methods. This is used during the initialization of the
//   Terminal, when we could be processing many "split-pane" commands _before_
//   we've ever laid out the Terminal for the first time. When this happens, the
//   Pane's don't have an actual size yet. However, we'd still like to figure
//   out how to do an "auto" split when these Panes are all laid out.
// - This method assumes that the Pane we're attempting to split is `target`,
//   and this method should be called on the root of a tree of Panes.
// - We'll walk down the tree attempting to find `target`. As we traverse the
//   tree, we'll reduce the size passed to each subsequent recursive call. The
//   size passed to this method represents how much space this Pane _will_ have
//   to use.
//   * If this pane is a leaf, and it's the pane we're looking for, use the
//     available space to calculate which direction to split in.
//   * If this pane is _any other leaf_, then just return nullopt, to indicate
//     that the `target` Pane is not down this branch.
//   * If this pane is a parent, calculate how much space our children will be
//     able to use, and recurse into them.
// Arguments:
// - target: The Pane we're attempting to split.
// - availableSpace: The theoretical space that's available for this pane to be able to split.
// Return Value:
// - nullopt if `target` is not this pane or a child of this pane, otherwise the
//   SplitState that `target` would use for an `Automatic` split given
//   `availableSpace`
std::optional<SplitState> Pane::PreCalculateAutoSplit(const std::shared_ptr<Pane> target,
                                                      const winrt::Windows::Foundation::Size availableSpace) const
{
    if (_IsLeaf())
    {
        if (target.get() == this)
        {
            //If this pane is a leaf, and it's the pane we're looking for, use
            //the available space to calculate which direction to split in.
            return availableSpace.Width > availableSpace.Height ? SplitState::Vertical : SplitState::Horizontal;
        }
        else
        {
            // If this pane is _any other leaf_, then just return nullopt, to
            // indicate that the `target` Pane is not down this branch.
            return std::nullopt;
        }
    }
    else
    {
        // If this pane is a parent, calculate how much space our children will
        // be able to use, and recurse into them.

        const bool isVerticalSplit = _splitState == SplitState::Vertical;
        const float firstWidth = isVerticalSplit ? (availableSpace.Width * _desiredSplitPosition) : availableSpace.Width;
        const float secondWidth = isVerticalSplit ? (availableSpace.Width - firstWidth) : availableSpace.Width;
        const float firstHeight = !isVerticalSplit ? (availableSpace.Height * _desiredSplitPosition) : availableSpace.Height;
        const float secondHeight = !isVerticalSplit ? (availableSpace.Height - firstHeight) : availableSpace.Height;

        const auto firstResult = _firstChild->PreCalculateAutoSplit(target, { firstWidth, firstHeight });
        return firstResult.has_value() ? firstResult : _secondChild->PreCalculateAutoSplit(target, { secondWidth, secondHeight });
    }

    // We should not possibly be getting here - both the above branches should
    // return a value.
    FAIL_FAST();
}

// Method Description:
// - Returns true if the pane or one of its descendants is read-only
bool Pane::ContainsReadOnly() const
{
    return _IsLeaf() ? _control.ReadOnly() : (_firstChild->ContainsReadOnly() || _secondChild->ContainsReadOnly());
}

// Method Description:
// - If we're a parent, place the taskbar state for all our leaves into the
//   provided vector.
// - If we're a leaf, place our own state into the vector.
// Arguments:
// - states: a vector that will receive all the states of all leaves in the tree
// Return Value:
// - <none>
void Pane::CollectTaskbarStates(std::vector<winrt::TerminalApp::TaskbarState>& states)
{
    if (_IsLeaf())
    {
        auto tbState{ winrt::make<winrt::TerminalApp::implementation::TaskbarState>(_control.TaskbarState(),
                                                                                    _control.TaskbarProgress()) };
        states.push_back(tbState);
    }
    else
    {
        _firstChild->CollectTaskbarStates(states);
        _secondChild->CollectTaskbarStates(states);
    }
}

DEFINE_EVENT(Pane, GotFocus, _GotFocusHandlers, winrt::delegate<std::shared_ptr<Pane>>);
DEFINE_EVENT(Pane, LostFocus, _LostFocusHandlers, winrt::delegate<std::shared_ptr<Pane>>);
DEFINE_EVENT(Pane, PaneRaiseBell, _PaneRaiseBellHandlers, winrt::Windows::Foundation::EventHandler<bool>);
DEFINE_EVENT(Pane, Detached, _PaneDetachedHandlers, winrt::delegate<std::shared_ptr<Pane>>);
