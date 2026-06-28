// partdesc.cpp — partition descriptor cache implementation.
//
// Converts the public API of PostgreSQL's src/backend/partitioning/partdesc.c
// to C++20. PostgreSQL caches PartitionDescData inside the relcache so that
// subsequent accesses to a partitioned table skip the catalog scan. MyToyDB
// keeps the same lookup API but uses an explicit std::map<oid, PartitionDesc>
// cache until the relcache layer is wired up.
//
// All storage is owned by the cache via std::unique_ptr, so PartitionDesc
// pointers returned to callers remain stable across map mutations
// (std::map is node-based).

#include "mytoydb/partitioning/partdesc.hpp"

#include <utility>

#include "mytoydb/common/error/elog.hpp"

namespace mytoydb::partitioning {

namespace {

// Process-wide cache. Defaults to nullptr; tests set it via
// SetPartitionDescriptorCache. In a full implementation this would be
// initialized during bootstrap (Phase 12) and consulted via the relcache.
PartitionDescriptorCache* g_partition_descriptor_cache = nullptr;

}  // namespace

// --- PartitionDescriptorCache ---

PartitionDesc PartitionDescriptorCache::Create(Oid parent_oid,
                                               std::vector<PartitionBoundSpec> specs,
                                               std::vector<Oid> partition_oids,
                                               std::vector<bool> is_leaf,
                                               PartitionStrategy strategy) {
    auto desc = std::make_unique<PartitionDescData>();
    desc->nparts = static_cast<int>(specs.size());
    desc->oids = std::move(partition_oids);
    desc->is_leaf = std::move(is_leaf);
    desc->specs = specs;
    desc->boundinfo = partition_bounds_create(specs, strategy);

    // detached_exist is always false in our simplified model — we do not
    // track DETACH PENDING partitions yet.
    desc->detached_exist = false;

    // Sanity-check sizes.
    if (desc->oids.size() != specs.size() || desc->is_leaf.size() != specs.size()) {
        ereport(error::LogLevel::kError,
                "PartitionDescriptorCache::Create: size mismatch between specs "
                "and oids/is_leaf arrays");
    }

    // Insert (or replace) the entry. raw pointer is stable across map
    // mutations because std::map nodes are not relocated.
    PartitionDesc raw = desc.get();
    entries_[parent_oid] = std::move(desc);
    return raw;
}

PartitionDesc PartitionDescriptorCache::Lookup(Oid parent_oid) const {
    auto it = entries_.find(parent_oid);
    if (it == entries_.end()) {
        return nullptr;
    }
    return it->second.get();
}

PartitionDesc PartitionDescriptorCache::LookupPartitionByOid(Oid partition_oid) const {
    for (const auto& [parent_oid, desc] : entries_) {
        for (Oid oid : desc->oids) {
            if (oid == partition_oid) {
                return desc.get();
            }
        }
    }
    return nullptr;
}

bool PartitionDescriptorCache::Invalidate(Oid parent_oid) {
    return entries_.erase(parent_oid) > 0;
}

// --- Global accessors ---

PartitionDescriptorCache* GetPartitionDescriptorCache() {
    return g_partition_descriptor_cache;
}

void SetPartitionDescriptorCache(PartitionDescriptorCache* cache) {
    g_partition_descriptor_cache = cache;
}

// --- Convenience wrappers (PostgreSQL-style API) ---

PartitionDesc CreatePartitionDesc(Oid parent_oid, const std::vector<PartitionBoundSpec>& specs,
                                  const std::vector<Oid>& partition_oids,
                                  const std::vector<bool>& is_leaf, PartitionStrategy strategy) {
    if (g_partition_descriptor_cache == nullptr) {
        ereport(error::LogLevel::kError,
                "CreatePartitionDesc: no global PartitionDescriptorCache set");
    }
    return g_partition_descriptor_cache->Create(parent_oid, specs, partition_oids, is_leaf,
                                                strategy);
}

PartitionDesc LookupPartitionByParentOid(Oid parent_oid) {
    if (g_partition_descriptor_cache == nullptr) {
        return nullptr;
    }
    return g_partition_descriptor_cache->Lookup(parent_oid);
}

PartitionDesc RelationGetPartitionDesc(Oid parent_oid) {
    // Mirrors PostgreSQL's RelationGetPartitionDesc. The omit_detached flag
    // is unused in our simplified model.
    return LookupPartitionByParentOid(parent_oid);
}

Oid get_default_oid_from_partdesc(PartitionDesc partdesc) {
    if (partdesc == nullptr) {
        return 0;
    }
    if (partdesc->boundinfo.default_index < 0 ||
        partdesc->boundinfo.default_index >= partdesc->nparts) {
        return 0;
    }
    int idx = partdesc->boundinfo.default_index;
    if (idx < 0 || static_cast<std::size_t>(idx) >= partdesc->oids.size()) {
        return 0;
    }
    return partdesc->oids[static_cast<std::size_t>(idx)];
}

}  // namespace mytoydb::partitioning
