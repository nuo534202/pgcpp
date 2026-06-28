// partdesc.hpp — partition descriptor cache (M9 sub-task 15.20.2).
//
// Converts PostgreSQL's src/include/partitioning/partdesc.h and the public
// API of src/backend/partitioning/partdesc.c to C++20.
//
// PostgreSQL caches PartitionDescData in the relcache so that subsequent
// accesses to a partitioned table skip the catalog scan. The MyToyDB layer
// keeps the same lookup API but uses a std::map<oid, PartitionDesc> cache,
// populated lazily by CreatePartitionDesc. The cache is process-local; it
// is the caller's responsibility to invalidate it when partition metadata
// changes (mirroring PostgreSQL's relcache invalidation).

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "pgcpp/partitioning/partbounds.hpp"

namespace mytoydb::partitioning {

// PartitionDescData — descriptor for a partitioned table's children.
// Mirrors PostgreSQL's struct PartitionDescData.
//
//   oids       — partition OIDs in bound order.
//   is_leaf    — true for each partition that is a leaf (not subpartitioned).
//   boundinfo  — merged bound info (may be nullptr if no partitions).
struct PartitionDescData {
    int nparts = 0;
    bool detached_exist = false;
    std::vector<Oid> oids;
    std::vector<bool> is_leaf;
    PartitionBoundInfoData boundinfo;

    // Per-partition bound specs (kept for tests / inspection; mirrors what
    // CreatePartitionDesc received).
    std::vector<PartitionBoundSpec> specs;
};

// Opaque handle preserved for API compatibility with PostgreSQL.
using PartitionDesc = PartitionDescData*;

// PartitionDescriptorCache — a simple in-memory cache keyed by parent OID.
// Mirrors what PostgreSQL achieves via the relcache. Inserting the same OID
// twice replaces the previous entry.
class PartitionDescriptorCache {
public:
    PartitionDescriptorCache() = default;
    ~PartitionDescriptorCache() = default;
    PartitionDescriptorCache(const PartitionDescriptorCache&) = delete;
    PartitionDescriptorCache& operator=(const PartitionDescriptorCache&) = delete;

    // Create or replace a descriptor entry. The cache takes a copy of the
    // descriptor data.
    PartitionDesc Create(Oid parent_oid, std::vector<PartitionBoundSpec> specs,
                         std::vector<Oid> partition_oids, std::vector<bool> is_leaf,
                         PartitionStrategy strategy);

    // Look up a descriptor by parent OID. Returns nullptr on miss.
    PartitionDesc Lookup(Oid parent_oid) const;

    // Look up a single partition (member) by its OID across all cached
    // descriptors. Returns the parent descriptor that owns it, or nullptr.
    PartitionDesc LookupPartitionByOid(Oid partition_oid) const;

    // Number of cached descriptors.
    std::size_t Size() const { return entries_.size(); }

    // Drop a single entry. Returns true if removed, false if not present.
    bool Invalidate(Oid parent_oid);

    // Drop all entries.
    void Clear() { entries_.clear(); }

private:
    // Owned storage. We use std::unique_ptr so PartitionDesc pointers
    // returned to callers remain stable across map mutations (std::map
    // node-based stability).
    std::map<Oid, std::unique_ptr<PartitionDescData>> entries_;
};

// --- Global accessor (PostgreSQL-style) ---
//
// PostgreSQL reaches the PartitionDesc via the relcache
// (RelationGetPartitionDesc). MyToyDB exposes a process-wide
// PartitionDescriptorCache through these accessors until the relcache is
// wired up; tests typically replace the global with a fresh instance.

PartitionDescriptorCache* GetPartitionDescriptorCache();
void SetPartitionDescriptorCache(PartitionDescriptorCache* cache);

// Convenience: build a PartitionDesc from specs and store it in the global
// cache. Returns the stored descriptor pointer.
PartitionDesc CreatePartitionDesc(Oid parent_oid, const std::vector<PartitionBoundSpec>& specs,
                                  const std::vector<Oid>& partition_oids,
                                  const std::vector<bool>& is_leaf, PartitionStrategy strategy);

// Convenience: look up the PartitionDesc for a parent OID via the global
// cache.
PartitionDesc LookupPartitionByParentOid(Oid parent_oid);

// Convenience: fetch the PartitionDesc for a parent relation. Mirrors
// PostgreSQL's RelationGetPartitionDesc but takes a parent OID (since the
// Relation handle is not modelled here).
PartitionDesc RelationGetPartitionDesc(Oid parent_oid);

// get_default_oid_from_partdesc — return the OID of the default partition,
// or kInvalidOid (0) if there is no default partition.
Oid get_default_oid_from_partdesc(PartitionDesc partdesc);

}  // namespace mytoydb::partitioning
