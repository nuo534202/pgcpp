// freespace.cpp — Free Space Map (FSM) per-relation cache.
//
// Converted from PostgreSQL 15's src/backend/storage/freespace/freespace.c.
#include "pgcpp/storage/freespace/freespace.hpp"

#include <map>

namespace pgcpp::storage {

namespace {

// RelationFSM — per-relation map of block → category.
using RelationFSM = std::map<BlockNumber, uint8_t>;

// FsmMap — top-level map of RelFileNode → RelationFSM.
// We use std::map<RelFileNode, RelationFSM>; RelFileNode has a defaulted
// operator< via tuple comparison.
struct RelFileNodeCmp {
    bool operator()(const RelFileNode& a, const RelFileNode& b) const {
        if (a.spc_node != b.spc_node)
            return a.spc_node < b.spc_node;
        if (a.db_node != b.db_node)
            return a.db_node < b.db_node;
        return a.rel_node < b.rel_node;
    }
};

std::map<RelFileNode, RelationFSM, RelFileNodeCmp>& FsmMap() {
    static std::map<RelFileNode, RelationFSM, RelFileNodeCmp> m;
    return m;
}

uint8_t ClampCategory(uint8_t cat) {
    if (cat > kFsmFreeSpaceMax) {
        return kFsmFreeSpaceMax;
    }
    return cat;
}

}  // namespace

BlockNumber GetPageWithFreeSpace(const RelFileNode& rnode, uint8_t min_cat) {
    auto it = FsmMap().find(rnode);
    if (it == FsmMap().end()) {
        return kInvalidBlockNumber;
    }
    for (const auto& kv : it->second) {
        if (kv.second >= min_cat) {
            return kv.first;
        }
    }
    return kInvalidBlockNumber;
}

void RecordPageWithFreeSpace(const RelFileNode& rnode, BlockNumber block, uint8_t cat) {
    cat = ClampCategory(cat);
    auto& rel_fsm = FsmMap()[rnode];
    // PG drops entries with category 0 to save space; we keep them for tests
    // so that "block has 0 free space" is queryable. Comment matches PG.
    rel_fsm[block] = cat;
}

void FreeSpaceMapTruncateRel(const RelFileNode& rnode, BlockNumber nblocks) {
    auto it = FsmMap().find(rnode);
    if (it == FsmMap().end()) {
        return;
    }
    auto& rel_fsm = it->second;
    // Erase all entries with block >= nblocks.
    auto upper = rel_fsm.lower_bound(nblocks);
    rel_fsm.erase(upper, rel_fsm.end());
}

int FreeSpaceMapVacuumRel(const RelFileNode& rnode) {
    auto it = FsmMap().find(rnode);
    if (it == FsmMap().end()) {
        return 0;
    }
    int removed = static_cast<int>(it->second.size());
    FsmMap().erase(it);
    return removed;
}

int FreeSpaceMapDropRel(const RelFileNode& rnode) {
    return FreeSpaceMapVacuumRel(rnode);
}

void ResetFreeSpaceMap() {
    FsmMap().clear();
}

std::vector<FreeSpaceMapEntry> GetFreeSpaceMapForRel(const RelFileNode& rnode) {
    std::vector<FreeSpaceMapEntry> result;
    auto it = FsmMap().find(rnode);
    if (it == FsmMap().end()) {
        return result;
    }
    for (const auto& kv : it->second) {
        result.push_back({kv.first, kv.second});
    }
    return result;
}

uint8_t FreeSpaceCategoryForBytes(int bytes) {
    // PG uses 256 buckets over the [0, BLCKSZ) range with a 32-byte stride.
    // We use the same: cat = floor(bytes / 32), clamped to [0, kFsmFreeSpaceMax].
    if (bytes <= 0) {
        return 0;
    }
    int cat = bytes / 32;
    // Clamp BEFORE narrowing to uint8_t — values above 255 wrap otherwise.
    if (cat > kFsmFreeSpaceMax) {
        return static_cast<uint8_t>(kFsmFreeSpaceMax);
    }
    return static_cast<uint8_t>(cat);
}

int BytesForFreeSpaceCategory(uint8_t cat) {
    // cat is in [0, kFsmFreeSpaceMax]; returns the lower bound in bytes.
    return static_cast<int>(cat) * 32;
}

int NumFSMRelations() {
    return static_cast<int>(FsmMap().size());
}

}  // namespace pgcpp::storage
