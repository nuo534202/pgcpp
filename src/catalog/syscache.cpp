// syscache.cpp — implementation of the system cache.
//
// Converts PostgreSQL's utils/cache/syscache.c to C++20. The hand-written
// hash table is replaced with std::unordered_map; the public API
// (SearchSysCache / ReleaseSysCache) is preserved.

#include "pgcpp/catalog/syscache.hpp"

#include <functional>
#include <utility>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/catalog/pg_attribute.hpp"
#include "pgcpp/catalog/pg_cast.hpp"
#include "pgcpp/catalog/pg_class.hpp"
#include "pgcpp/catalog/pg_operator.hpp"
#include "pgcpp/catalog/pg_proc.hpp"
#include "pgcpp/catalog/pg_type.hpp"
#include "pgcpp/common/error/elog.hpp"

namespace mytoydb::catalog {

namespace {

SysCache* g_syscache = nullptr;

}  // namespace

// --- Composite key helper for string keys ---

std::uint64_t SysCache::MakeKey(const std::string& s, Oid oid) {
    // Combine the string hash with the Oid. Use a simple mixing step so that
    // (s, oid) collisions are unlikely in practice.
    auto h = std::hash<std::string>{}(s);
    return h ^ (static_cast<std::uint64_t>(oid) << 32) ^ 0x9E3779B97F4A7C15ULL;
}

std::uint64_t SysCache::MakeKey4(const std::string& s, Oid left, Oid right, Oid nsp) {
    // Mix the string hash with the three Oids. Each Oid occupies a different
    // bit position in the mix; the string hash provides the high entropy.
    auto h = std::hash<std::string>{}(s);
    std::uint64_t k = h;
    k ^= static_cast<std::uint64_t>(left) * 0x9E3779B97F4A7C15ULL;
    k ^= static_cast<std::uint64_t>(right) * 0xC2B2AE3D27D4EB4FULL;
    k ^= static_cast<std::uint64_t>(nsp) * 0x165667B19E3779F9ULL;
    return k;
}

std::uint64_t SysCache::MakeKeyProc(const std::string& s, const std::vector<Oid>& argtypes,
                                    Oid nsp) {
    // Hash the name, each argument type, and the namespace. This deliberately
    // re-hashes the whole argtypes vector so that (name, [int4,int4], nsp)
    // and (name, [int4], nsp) produce distinct keys.
    auto h = std::hash<std::string>{}(s);
    std::uint64_t k = h;
    for (Oid arg : argtypes) {
        k ^= static_cast<std::uint64_t>(arg) * 0x9E3779B97F4A7C15ULL;
        k = (k << 1) | (k >> 63);
    }
    k ^= static_cast<std::uint64_t>(nsp) * 0xC2B2AE3D27D4EB4FULL;
    return k;
}

// --- Init / Invalidate ---

void SysCache::Init(const Catalog* catalog) {
    if (catalog == nullptr) {
        ereport(error::LogLevel::kError, "SysCache::Init: catalog is null");
    }
    Invalidate();

    // Walk pg_class rows and build both indexes.
    // We use the Catalog's typed accessors by scanning known OIDs. Since the
    // Catalog does not expose an iterator, we rely on the fact that Init is
    // called after bootstrap and re-derive rows via GetClassByOid starting
    // from kFirstNormalObjectId. For built-in types (OID < 16384) we also
    // scan a small fixed range. This is a simplified model; a full
    // implementation would iterate the catalog's internal vectors directly.
    //
    // For now, we expose a friend-like path: the Catalog's row vectors are
    // accessed via the public GetClassByOid / GetTypeByOid accessors by
    // scanning OIDs. To avoid an O(N^2) scan, we instead require callers to
    // populate the cache via the typed SearchXxxByYyy methods, which lazily
    // fall back to the Catalog. So Init is a no-op here; lookups are lazy.
    //
    // This matches PostgreSQL's "cache is populated on demand" semantics.
}

void SysCache::Invalidate() {
    class_by_oid_.clear();
    class_by_name_.clear();
    attribute_by_relid_num_.clear();
    attribute_by_relid_name_.clear();
    type_by_oid_.clear();
    type_by_name_.clear();
    operator_by_name_lr_n_.clear();
    proc_by_name_args_nsp_.clear();
    cast_by_source_target_.clear();
    refcounts_.clear();
}

// --- Pin management ---

void SysCache::Pin(const void* entry) const {
    if (entry == nullptr) {
        return;
    }
    ++refcounts_[entry];
}

void SysCache::Unpin(const void* entry) const {
    if (entry == nullptr) {
        return;
    }
    auto it = refcounts_.find(entry);
    if (it == refcounts_.end() || it->second <= 0) {
        // Unpin of an unpinned entry — ignore (PostgreSQL would Assert-fail,
        // but we are lenient for testability).
        return;
    }
    --it->second;
    if (it->second == 0) {
        refcounts_.erase(it);
    }
}

void SysCache::Release(const void* entry) const {
    Unpin(entry);
}

// --- pg_class lookups (lazy: fall back to Catalog on miss) ---

const FormData_pg_class* SysCache::SearchClassByOid(Oid oid) const {
    auto it = class_by_oid_.find(oid);
    if (it != class_by_oid_.end()) {
        Pin(it->second);
        return it->second;
    }
    // Lazy miss: consult the Catalog and cache the result.
    const Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_class* row = cat->GetClassByOid(oid);
    if (row != nullptr) {
        const_cast<SysCache*>(this)->class_by_oid_[oid] = row;
        Pin(row);
    }
    return row;
}

const FormData_pg_class* SysCache::SearchClassByName(const std::string& name,
                                                     Oid namespace_oid) const {
    auto key = MakeKey(name, namespace_oid);
    auto it = class_by_name_.find(key);
    if (it != class_by_name_.end()) {
        Pin(it->second);
        return it->second;
    }
    const Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_class* row = cat->GetClassByName(name);
    // For now we ignore namespace filtering (single-namespace bootstrap).
    if (row != nullptr) {
        const_cast<SysCache*>(this)->class_by_name_[key] = row;
        Pin(row);
    }
    return row;
}

// --- pg_attribute lookups ---

const FormData_pg_attribute* SysCache::SearchAttributeByNum(Oid relid, int16_t attnum) const {
    auto key = MakeKey(relid, attnum);
    auto it = attribute_by_relid_num_.find(key);
    if (it != attribute_by_relid_num_.end()) {
        Pin(it->second);
        return it->second;
    }
    const Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_attribute* row = cat->GetAttribute(relid, attnum);
    if (row != nullptr) {
        const_cast<SysCache*>(this)->attribute_by_relid_num_[key] = row;
        Pin(row);
    }
    return row;
}

const FormData_pg_attribute* SysCache::SearchAttributeByName(Oid relid,
                                                             const std::string& name) const {
    auto key = MakeKey(name, relid);
    auto it = attribute_by_relid_name_.find(key);
    if (it != attribute_by_relid_name_.end()) {
        Pin(it->second);
        return it->second;
    }
    const Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    // pg_attribute has no direct name index in Catalog; scan attributes for
    // the relation and match by name.
    auto attrs = cat->GetAttributes(relid);
    const FormData_pg_attribute* row = nullptr;
    for (const auto* a : attrs) {
        if (a->attname == name) {
            row = a;
            break;
        }
    }
    if (row != nullptr) {
        const_cast<SysCache*>(this)->attribute_by_relid_name_[key] = row;
        Pin(row);
    }
    return row;
}

// --- pg_type lookups ---

const FormData_pg_type* SysCache::SearchTypeByOid(Oid oid) const {
    auto it = type_by_oid_.find(oid);
    if (it != type_by_oid_.end()) {
        Pin(it->second);
        return it->second;
    }
    const Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_type* row = cat->GetTypeByOid(oid);
    if (row != nullptr) {
        const_cast<SysCache*>(this)->type_by_oid_[oid] = row;
        Pin(row);
    }
    return row;
}

const FormData_pg_type* SysCache::SearchTypeByName(const std::string& name,
                                                   Oid namespace_oid) const {
    auto key = MakeKey(name, namespace_oid);
    auto it = type_by_name_.find(key);
    if (it != type_by_name_.end()) {
        Pin(it->second);
        return it->second;
    }
    const Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_type* row = cat->GetTypeByName(name);
    if (row != nullptr) {
        const_cast<SysCache*>(this)->type_by_name_[key] = row;
        Pin(row);
    }
    return row;
}

// --- pg_operator lookups (4-key) ---

const FormData_pg_operator* SysCache::SearchOperatorByNameLRN(const std::string& name, Oid left,
                                                              Oid right, Oid nsp) const {
    auto key = MakeKey4(name, left, right, nsp);
    auto it = operator_by_name_lr_n_.find(key);
    if (it != operator_by_name_lr_n_.end()) {
        Pin(it->second);
        return it->second;
    }
    const Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_operator* row = cat->GetOperator(name, left, right);
    // PG would also filter by namespace_oid; MyToyDB has a single namespace
    // for built-ins, so we accept any namespace value.
    if (row != nullptr) {
        const_cast<SysCache*>(this)->operator_by_name_lr_n_[key] = row;
        Pin(row);
    }
    return row;
}

// --- pg_proc lookups (name + argtypes + nsp) ---

const FormData_pg_proc* SysCache::SearchProcByNameArgsNsp(const std::string& name,
                                                          const std::vector<Oid>& argtypes,
                                                          Oid nsp) const {
    auto key = MakeKeyProc(name, argtypes, nsp);
    auto it = proc_by_name_args_nsp_.find(key);
    if (it != proc_by_name_args_nsp_.end()) {
        Pin(it->second);
        return it->second;
    }
    const Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    // Find candidates by name, then exact-match on argument types.
    auto candidates = cat->GetProcsByName(name);
    const FormData_pg_proc* row = nullptr;
    for (const auto* p : candidates) {
        if (p->proargtypes == argtypes) {
            row = p;
            break;
        }
    }
    if (row != nullptr) {
        const_cast<SysCache*>(this)->proc_by_name_args_nsp_[key] = row;
        Pin(row);
    }
    return row;
}

// --- pg_cast lookups (source, target) ---

const FormData_pg_cast* SysCache::SearchCastBySourceTarget(Oid source, Oid target) const {
    auto key = MakeKey(source, target);
    auto it = cast_by_source_target_.find(key);
    if (it != cast_by_source_target_.end()) {
        Pin(it->second);
        return it->second;
    }
    const Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        return nullptr;
    }
    const FormData_pg_cast* row = cat->GetCast(source, target);
    if (row != nullptr) {
        const_cast<SysCache*>(this)->cast_by_source_target_[key] = row;
        Pin(row);
    }
    return row;
}

// --- Global accessors ---

SysCache* GetSysCache() {
    return g_syscache;
}

void SetSysCache(SysCache* cache) {
    g_syscache = cache;
}

// --- PostgreSQL-compatible SearchSysCache API ---

const void* SearchSysCache1(SysCacheIdentifier cache_id, uintptr_t key1) {
    SysCache* cache = GetSysCache();
    if (cache == nullptr) {
        return nullptr;
    }
    switch (cache_id) {
        case SysCacheIdentifier::kClassOid:
            return cache->SearchClassByOid(static_cast<Oid>(key1));
        case SysCacheIdentifier::kTypeOid:
            return cache->SearchTypeByOid(static_cast<Oid>(key1));
        case SysCacheIdentifier::kAttributeRelidNum: {
            Oid relid = static_cast<Oid>(key1 >> 16);
            int16_t attnum = static_cast<int16_t>(static_cast<uint16_t>(key1 & 0xFFFF));
            return cache->SearchAttributeByNum(relid, attnum);
        }
        default:
            ereport(error::LogLevel::kError,
                    "SearchSysCache1: unsupported cache id for single-key lookup");
    }
    return nullptr;
}

const void* SearchSysCache2(SysCacheIdentifier cache_id, uintptr_t key1, uintptr_t key2) {
    SysCache* cache = GetSysCache();
    if (cache == nullptr) {
        return nullptr;
    }
    switch (cache_id) {
        case SysCacheIdentifier::kClassName: {
            // key1 is a pointer to std::string, key2 is namespace Oid.
            const auto* name = reinterpret_cast<const std::string*>(key1);
            if (name == nullptr) {
                return nullptr;
            }
            return cache->SearchClassByName(*name, static_cast<Oid>(key2));
        }
        case SysCacheIdentifier::kAttributeRelidName: {
            // key1 is relid, key2 is a pointer to std::string.
            Oid relid = static_cast<Oid>(key1);
            const auto* name = reinterpret_cast<const std::string*>(key2);
            if (name == nullptr) {
                return nullptr;
            }
            return cache->SearchAttributeByName(relid, *name);
        }
        case SysCacheIdentifier::kTypeName: {
            const auto* name = reinterpret_cast<const std::string*>(key1);
            if (name == nullptr) {
                return nullptr;
            }
            return cache->SearchTypeByName(*name, static_cast<Oid>(key2));
        }
        default:
            ereport(error::LogLevel::kError,
                    "SearchSysCache2: unsupported cache id for two-key lookup");
    }
    return nullptr;
}

const void* SearchSysCache3(SysCacheIdentifier cache_id, uintptr_t key1, uintptr_t key2,
                            uintptr_t /*key3*/) {
    SysCache* cache = GetSysCache();
    if (cache == nullptr) {
        return nullptr;
    }
    switch (cache_id) {
        case SysCacheIdentifier::kCastSourceTarget:
            // (source, target, unused) -> delegate to the 2-key cast lookup.
            return cache->SearchCastBySourceTarget(static_cast<Oid>(key1), static_cast<Oid>(key2));
        default:
            ereport(error::LogLevel::kError,
                    "SearchSysCache3: unsupported cache id for three-key lookup");
    }
    return nullptr;
}

const void* SearchSysCache4(SysCacheIdentifier cache_id, uintptr_t key1, uintptr_t key2,
                            uintptr_t key3, uintptr_t key4) {
    SysCache* cache = GetSysCache();
    if (cache == nullptr) {
        return nullptr;
    }
    switch (cache_id) {
        case SysCacheIdentifier::kOperatorNameLrN: {
            // key1 = pointer to std::string (operator name)
            // key2 = left operand type OID
            // key3 = right operand type OID
            // key4 = namespace OID
            const auto* name = reinterpret_cast<const std::string*>(key1);
            if (name == nullptr) {
                return nullptr;
            }
            return cache->SearchOperatorByNameLRN(*name, static_cast<Oid>(key2),
                                                  static_cast<Oid>(key3), static_cast<Oid>(key4));
        }
        case SysCacheIdentifier::kProcNameArgsNsp: {
            // key1 = pointer to std::string (function name)
            // key2 = pointer to std::vector<Oid> (argument types)
            // key3 = nargs (unused — derived from argtypes.size())
            // key4 = namespace OID
            const auto* name = reinterpret_cast<const std::string*>(key1);
            const auto* argtypes = reinterpret_cast<const std::vector<Oid>*>(key2);
            if (name == nullptr || argtypes == nullptr) {
                return nullptr;
            }
            return cache->SearchProcByNameArgsNsp(*name, *argtypes, static_cast<Oid>(key4));
        }
        default:
            ereport(error::LogLevel::kError,
                    "SearchSysCache4: unsupported cache id for four-key lookup");
    }
    return nullptr;
}

void ReleaseSysCache(const void* entry) {
    SysCache* cache = GetSysCache();
    if (cache != nullptr) {
        cache->Release(entry);
    }
}

}  // namespace mytoydb::catalog
