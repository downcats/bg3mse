#include <stdafx.h>
#include <GameDefinitions/Osiris.h>

#include "StoryContainersNorbyte.h"

BEGIN_NS(mac::test)

namespace {

#define HOLDS(cond) do { if (!(cond)) { findings += #cond; findings += " (line " + std::to_string(__LINE__) + ")\n"; } } while (0)

// Kept out of line and opaque to the caller's optimiser, so that the list
// below is not folded away before it is built.
__attribute__((noinline)) size_t Count(TuplePtrLL* tuple)
{
    // As OsirisCallbackManager::RunHandler counts a tuple's values.
    size_t count = 0;
    auto node = tuple->Items.Head->Next;
    while (node != tuple->Items.Head) {
        node = node->Next;
        count++;
    }
    return count;
}

}

std::string CheckStoryListsBuilt()
{
    std::string findings;

    // As OsiFunction::OsiInsert builds the tuple it inserts.
    constexpr uint32_t values = 5;
    TypedValue* items[values];
    for (uint32_t i = 0; i < values; i++) items[i] = reinterpret_cast<TypedValue*>(static_cast<uintptr_t>(0x1000 + 0x10 * i));
    ListNode<TypedValue*> nodes[values + 1];

    TuplePtrLL tuple;
    auto& args = tuple.Items;
    args.Init(nodes);
    HOLDS(args.Head->Next == args.Head && args.Head->Head == args.Head && args.Size == 0);
    HOLDS(Count(&tuple) == 0);

    auto prev = args.Head;
    for (uint32_t i = 0; i < values; i++) {
        auto node = nodes + i + 1;
        args.Insert(items[i], node, prev);
        prev = node;
    }

    // Read back as the list's own members, as a node, and as his readers walk it.
    HOLDS(args.Size == values && args.First == &nodes[1] && args.Last == &nodes[values]);
    HOLDS(args.Head->Next == &nodes[1] && args.Head->Head == &nodes[values]);
    HOLDS(Count(&tuple) == values);
    uint32_t seen = 0;
    auto head = args.Head;
    for (auto node = head->Next; node != head; node = node->Next) {
        HOLDS(seen < values && node->Item == items[seen]);
        seen++;
    }
    HOLDS(seen == values);
    // And backwards: every node's previous node, down to the end node.
    seen = values;
    for (auto node = head->Head; node != head; node = node->Head) {
        seen--;
        HOLDS(node->Item == items[seen]);
    }
    HOLDS(seen == 0);

    // Emptied and built again in the same place.
    args.Init(nodes);
    HOLDS(args.Head->Next == args.Head && args.Size == 0 && Count(&tuple) == 0);
    args.Insert(items[2], nodes + 1, args.Head);
    HOLDS(args.First == &nodes[1] && args.Last == &nodes[1] && nodes[1].Next == args.Head && nodes[1].Head == args.Head && Count(&tuple) == 1);
    return findings;
}

END_NS()
