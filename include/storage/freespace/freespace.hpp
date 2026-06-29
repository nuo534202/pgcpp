// freespace.h — Free Space Map (FSM) per-relation cache.
//
// Converted from PostgreSQL 15's src/include/storage/freespace.h and
// src/backend/storage/freespace/freespace.c.
//
// The FSM tracks the free space available on each heap page of a relation
// so that inserts can find a page with enough room without scanning every
// page. PG stores the FSM on disk in the rel_fsm fork, organized as a
// multi-level binary tree of FSM pages.
//
// pgcpp keeps the FSM in memory as a per-relation std::map<BlockNumber,
// uint8_t> of free-space values. The API mirrors PG's GetPageWithFreeSpace
// / RecordPageWithFreeSpace surface.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

#include "storage/block.hpp"
#include "storage/relfilenode.hpp"

namespace pgcpp::storage {

// FSM_FREE_SPACE_MAX — the maximum free-space value tracked by the FSM
// (matches PG's BLCKSZ - 1 in bytes, rounded to FSM's 256-step resolution).
constexpr int kFsmFreeSpaceMax = 255;

// FSMAddress — logical address inside a relation's FSM (leaf page + slot).
// In pgcpp this is just a bookkeeping struct; the leaf/slot split mirrors
// PG's on-disk layout for fidelity.
struct FSMAddress {
    int leaf = 0;  // leaf page number (within the relation's FSM)
    int slot = 0;  // slot inside the leaf page

    bool operator==(const FSMAddress&) const = default;
};

// FreeSpaceMapEntry — recorded free space for one heap block.
struct FreeSpaceMapEntry {
    BlockNumber block;
    uint8_t cat;  // category value 0..kFsmFreeSpaceMax
};

// GetPageWithFreeSpace — return the first block in the relation whose
// recorded free-space category is >= min_cat, or kInvalidBlockNumber if
// none. PG's GetPageWithFreeSpace searches the on-disk FSM; we search the
// in-memory map.
BlockNumber GetPageWithFreeSpace(const RelFileNode& rnode, uint8_t min_cat);

// RecordPageWithFreeSpace — record the free-space category for a block.
// Category values above kFsmFreeSpaceMax are clamped to kFsmFreeSpaceMax.
void RecordPageWithFreeSpace(const RelFileNode& rnode, BlockNumber block, uint8_t cat);

// FreeSpaceMapTruncateRel — drop all FSM entries for blocks >= nblocks.
void FreeSpaceMapTruncateRel(const RelFileNode& rnode, BlockNumber nblocks);

// FreeSpaceMapVacuumRel — clear FSM entries for the relation (used by VACUUM
// before it rebuilds the map). Returns the number of entries removed.
int FreeSpaceMapVacuumRel(const RelFileNode& rnode);

// FreeSpaceMapDropRel — drop all FSM entries for a relation (DROP TABLE).
int FreeSpaceMapDropRel(const RelFileNode& rnode);

// ResetFreeSpaceMap — drop all FSM entries (used by tests).
void ResetFreeSpaceMap();

// GetFreeSpaceMapForRel — snapshot of a relation's FSM entries (for tests).
std::vector<FreeSpaceMapEntry> GetFreeSpaceMapForRel(const RelFileNode& rnode);

// FreeSpaceCategoryForBytes — convert a byte count to an FSM category.
// PG rounds down to the next category boundary so GetPageWithFreeSpace
// returns pages with at least the requested bytes.
uint8_t FreeSpaceCategoryForBytes(int bytes);

// BytesForFreeSpaceCategory — inverse of FreeSpaceCategoryForBytes.
int BytesForFreeSpaceCategory(uint8_t cat);

// NumFSMRelations — count of relations with FSM entries.
int NumFSMRelations();

}  // namespace pgcpp::storage
