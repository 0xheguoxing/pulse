// action_inventory.cpp - Compile-time checks of the action list.
//
// Every keyboard action is linked once and in order, every link names a
// listed action (or why it has none), and no id or command appears twice.
// MenuCmd values cannot be enumerated in C++; tools/actions/check_actions.py
// checks that every one of them is linked.
#include "action_inventory.h"

#include <iterator>

namespace pulse::app::actions {
namespace {

constexpr bool KeyLinksInOrder() {
    if (std::size(kKeyActionLinks) != static_cast<size_t>(kKeyActionCount)) return false;
    for (int i = 0; i < kKeyActionCount; ++i)
        if (static_cast<int>(kKeyActionLinks[i].key) != i) return false;
    return true;
}

constexpr bool LinkKnown(const ActionLink& link) {
    return link.id.empty() ? !link.why_none.empty() : FindAction(link.id) != nullptr;
}

constexpr bool AllLinksKnown() {
    for (const KeyActionLink& k : kKeyActionLinks)
        if (!LinkKnown(k.link)) return false;
    for (const MenuCmdLink& m : kMenuCmdLinks)
        if (!LinkKnown(m.link)) return false;
    return true;
}

constexpr bool ActionIdsUnique() {
    const size_t n = std::size(kActions);
    for (size_t i = 0; i < n; ++i) {
        if (kActions[i].id.empty()) return false;
        for (size_t j = i + 1; j < n; ++j)
            if (kActions[i].id == kActions[j].id) return false;
    }
    return true;
}

constexpr bool MenuCommandsUnique() {
    const size_t n = std::size(kMenuCmdLinks);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = i + 1; j < n; ++j)
            if (kMenuCmdLinks[i].command == kMenuCmdLinks[j].command) return false;
    return true;
}

static_assert(KeyLinksInOrder(), "kKeyActionLinks: one entry per KeyAction, in enum order");
static_assert(AllLinksKnown(), "a link names an action missing from kActions, or has no reason");
static_assert(ActionIdsUnique(), "kActions: an id is empty or listed twice");
static_assert(MenuCommandsUnique(), "kMenuCmdLinks: a command is linked twice");

} // namespace
} // namespace pulse::app::actions
