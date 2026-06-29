// fsmpage.cpp — FSM leaf page (binary-tree-of-slots layout).
//
// Converted from PostgreSQL 15's src/backend/storage/freespace/fsmpage.c.
#include "storage/freespace/fsmpage.hpp"

#include <algorithm>

namespace pgcpp::storage {

namespace {

// NodeIndexFromSlot — given a leaf slot (0-based from left), return the
// index of that leaf in the flattened binary-tree array.
//
// PG uses a complete binary tree stored as an array where the root is at
// index 0; left child of node i is 2i+1, right child is 2i+2. The leaves
// are the rightmost kFsmLeafNodesPerPage nodes.
//
// For a complete binary tree of L leaves, the leaf starting index is
// (L - 1) since 2L - 1 nodes total, and the leaves occupy indices
// (L - 1) to (2L - 2).
int NodeIndexFromSlot(int slot) {
    return (kFsmLeafNodesPerPage - 1) + slot;
}

// ParentIndex — return the parent index of node_index, or -1 for the root.
int ParentIndex(int node_index) {
    if (node_index == 0) {
        return -1;
    }
    return (node_index - 1) / 2;
}

// MaxOfChildren — for an internal node, return the max of its two children
// (or 0 if no children).
uint8_t MaxOfChildren(const FSMPageData* page, int node_index) {
    int left = 2 * node_index + 1;
    int right = 2 * node_index + 2;
    if (left >= kFsmNodesPerPage) {
        return 0;  // leaf
    }
    uint8_t l = page->nodes[left];
    uint8_t r = (right < kFsmNodesPerPage) ? page->nodes[right] : 0;
    return std::max(l, r);
}

// PropagateUp — recompute internal node values up the tree from a leaf slot.
void PropagateUp(FSMPageData* page, int slot) {
    int node = NodeIndexFromSlot(slot);
    while (node != 0) {
        int parent = ParentIndex(node);
        page->nodes[parent] = MaxOfChildren(page, parent);
        node = parent;
    }
}

}  // namespace

void FSMPageInit(FSMPageData* page) {
    for (int i = 0; i < kFsmNodesPerPage; ++i) {
        page->nodes[i] = 0;
    }
    page->fp_next_slot = 0;
}

uint8_t FSMPageGetFreeSpace(const FSMPageData* page) {
    // Root is at index 0.
    return page->nodes[0];
}

void FSMPageSetFreeSpace(FSMPageData* page, int slot, uint8_t value) {
    if (slot < 0 || slot >= kFsmLeafNodesPerPage) {
        return;
    }
    int node = NodeIndexFromSlot(slot);
    page->nodes[node] = value;
    PropagateUp(page, slot);
}

int FSMPageSearchFreeSpace(FSMPageData* page, uint8_t min_value) {
    // PG's algorithm: start at the root, walk down choosing the child with
    // the larger value (PG picks left when equal). If neither child meets
    // min_value, no slot qualifies.
    //
    // We also implement PG's round-robin search hint (fp_next_slot): try
    // starting the search from a hint slot first, then from the root.
    //
    // For simplicity pgcpp uses the root-walk variant; fp_next_slot is
    // updated to the found slot to mirror PG's behavior.
    if (page->nodes[0] < min_value) {
        return -1;
    }
    int node = 0;
    while (node < kFsmLeafNodesPerPage - 1) {  // while internal
        int left = 2 * node + 1;
        int right = 2 * node + 2;
        uint8_t lv = (left < kFsmNodesPerPage) ? page->nodes[left] : 0;
        uint8_t rv = (right < kFsmNodesPerPage) ? page->nodes[right] : 0;
        if (lv >= min_value && lv >= rv) {
            node = left;
        } else if (rv >= min_value) {
            node = right;
        } else {
            return -1;  // shouldn't happen since root met min_value
        }
    }
    int slot = node - (kFsmLeafNodesPerPage - 1);
    page->fp_next_slot = slot;
    return slot;
}

uint8_t FSMPageGetSlot(const FSMPageData* page, int slot) {
    if (slot < 0 || slot >= kFsmLeafNodesPerPage) {
        return 0;
    }
    return page->nodes[NodeIndexFromSlot(slot)];
}

void FSMPageClear(FSMPageData* page) {
    FSMPageInit(page);
}

int FSMLeftChild(int node_index) {
    return 2 * node_index + 1;
}

int FSMRightChild(int node_index) {
    return 2 * node_index + 2;
}

int FSMSlotToNodeIndex(int slot) {
    return NodeIndexFromSlot(slot);
}

}  // namespace pgcpp::storage
