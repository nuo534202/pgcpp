// fsmpage.h — FSM leaf page (binary-tree-of-slots layout).
//
// Converted from PostgreSQL 15's src/include/storage/freespace/fsmpage.h and
// src/backend/storage/freespace/fsmpage.c.
//
// A FSM page is a fixed-size array of category slots organized as a binary
// tree: each non-leaf slot stores the max of its two children so that a
// search for a slot with at least X free bytes can walk down the tree in
// log(N) time. PG stores this array inside a heap page (FSM fork).
//
// MyToyDB models the FSM page as an in-memory struct so we can unit-test the
// tree-search algorithm without a real on-disk page.
#pragma once

#include <cstdint>

namespace mytoydb::storage {

// LeafNodes — number of leaf slots in one FSM page (PG default: 162).
constexpr int kFsmLeafNodesPerPage = 162;

// Nodes — total nodes in the binary tree (leaf + internal).
constexpr int kFsmNodesPerPage = 2 * kFsmLeafNodesPerPage - 1;

// FSMPageData — in-memory representation of one FSM page.
struct FSMPageData {
    uint8_t nodes[kFsmNodesPerPage] = {0};  // node values: leaves + internal
    int fp_next_slot = 0;                   // round-robin search hint
};

// FSMPageInit — initialize a freshly-allocated FSM page to all-zero.
void FSMPageInit(FSMPageData* page);

// FSMPageGetFreeSpace — return the maximum free-space category in the page
// (the value at the root node).
uint8_t FSMPageGetFreeSpace(const FSMPageData* page);

// FSMPageSetFreeSpace — set the category value for a specific leaf slot.
// Also propagates the change up the tree (each parent = max of its children).
void FSMPageSetFreeSpace(FSMPageData* page, int slot, uint8_t value);

// FSMPageSearchFreeSpace — find a leaf slot with category >= min_value.
// Returns the slot index (0-based), or -1 if none.
// Implements PG's round-robin search starting from fp_next_slot.
int FSMPageSearchFreeSpace(FSMPageData* page, uint8_t min_value);

// FSMPageGetSlot — read the category value of a specific leaf slot.
uint8_t FSMPageGetSlot(const FSMPageData* page, int slot);

// FSMPageClear — reset all slots to 0 (used by tests / vacuum).
void FSMPageClear(FSMPageData* page);

// FSMLeftChild / FSMRightChild — child-node index helpers (used in tests).
int FSMLeftChild(int node_index);
int FSMRightChild(int node_index);

// FSMSlotToNodeIndex — convert a leaf slot number to a node-tree index.
int FSMSlotToNodeIndex(int slot);

}  // namespace mytoydb::storage
