// catalog.cpp — implementation of the in-memory system catalog.
//
// Converts PostgreSQL's catalog/indexing.c + catalog/heap.c catalog-tuple
// helpers to C++20. Row storage is a std::vector of palloc-allocated pointers,
// preserving PostgreSQL's "rows live in a long-lived memory context" model.

#include "catalog/catalog.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <utility>

#include "catalog/pg_aggregate.hpp"
#include "catalog/pg_attribute.hpp"
#include "catalog/pg_cast.hpp"
#include "catalog/pg_class.hpp"
#include "catalog/pg_collation.hpp"
#include "catalog/pg_operator.hpp"
#include "catalog/pg_proc.hpp"
#include "catalog/pg_type.hpp"
#include "common/containers/node.hpp"
#include "common/error/elog.hpp"
#include "common/memory/memory_context.hpp"

namespace pgcpp::catalog {

namespace {

// Process-wide catalog pointer. Defaults to nullptr; tests set it via
// SetCatalog. In a full implementation this would be initialized during
// bootstrap (Phase 12).
Catalog* g_catalog = nullptr;

}  // namespace

// --- Catalog: pg_class ---

Oid Catalog::InsertClass(FormData_pg_class* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertClass: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    // Reject duplicate OIDs.
    for (const auto* existing : pg_class_rows_) {
        if (existing->oid == row->oid) {
            ereport(error::LogLevel::kError, "Catalog::InsertClass: duplicate OID");
        }
    }
    pg_class_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_class* Catalog::GetClassByOid(Oid oid) const {
    for (const auto* row : pg_class_rows_) {
        if (row->oid == oid) {
            return row;
        }
    }
    return nullptr;
}

const FormData_pg_class* Catalog::GetClassByName(const std::string& name) const {
    for (const auto* row : pg_class_rows_) {
        if (row->relname == name) {
            return row;
        }
    }
    return nullptr;
}

bool Catalog::UpdateClass(Oid oid, const FormData_pg_class* new_row) {
    for (auto*& row : pg_class_rows_) {
        if (row->oid == oid) {
            // Copy fields from new_row into the existing row (in-place update,
            // preserving the row pointer identity that SysCache may pin).
            row->relname = new_row->relname;
            row->relnamespace = new_row->relnamespace;
            row->reltype = new_row->reltype;
            row->reloftype = new_row->reloftype;
            row->relowner = new_row->relowner;
            row->relam = new_row->relam;
            row->relfilenode = new_row->relfilenode;
            row->reltablespace = new_row->reltablespace;
            row->relpages = new_row->relpages;
            row->reltuples = new_row->reltuples;
            row->reltoastrelid = new_row->reltoastrelid;
            row->relhasindex = new_row->relhasindex;
            row->relisshared = new_row->relisshared;
            row->relpersistence = new_row->relpersistence;
            row->relkind = new_row->relkind;
            row->relnatts = new_row->relnatts;
            row->relchecks = new_row->relchecks;
            row->relhasrules = new_row->relhasrules;
            row->relhastriggers = new_row->relhastriggers;
            row->relrowsecurity = new_row->relrowsecurity;
            row->relforcerowsecurity = new_row->relforcerowsecurity;
            row->relispopulated = new_row->relispopulated;
            row->relreplident = new_row->relreplident;
            row->relispartition = new_row->relispartition;
            row->relfrozenxid = new_row->relfrozenxid;
            row->relminmxid = new_row->relminmxid;
            return true;
        }
    }
    return false;
}

bool Catalog::DeleteClass(Oid oid) {
    auto it = std::find_if(pg_class_rows_.begin(), pg_class_rows_.end(),
                           [oid](const FormData_pg_class* r) { return r->oid == oid; });
    if (it == pg_class_rows_.end()) {
        return false;
    }
    pg_class_rows_.erase(it);
    return true;
}

// --- Catalog: pg_attribute ---

void Catalog::InsertAttribute(FormData_pg_attribute* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertAttribute: row is null");
    }
    pg_attribute_rows_.push_back(row);
}

const FormData_pg_attribute* Catalog::GetAttribute(Oid relid, int16_t attnum) const {
    for (const auto* row : pg_attribute_rows_) {
        if (row->attrelid == relid && row->attnum == attnum) {
            return row;
        }
    }
    return nullptr;
}

std::vector<const FormData_pg_attribute*> Catalog::GetAttributes(Oid relid) const {
    std::vector<const FormData_pg_attribute*> result;
    for (const auto* row : pg_attribute_rows_) {
        if (row->attrelid == relid) {
            result.push_back(row);
        }
    }
    // Sort by attnum (PostgreSQL keeps attributes in attnum order).
    std::sort(result.begin(), result.end(),
              [](const FormData_pg_attribute* a, const FormData_pg_attribute* b) {
                  return a->attnum < b->attnum;
              });
    return result;
}

std::size_t Catalog::DeleteAttributes(Oid relid) {
    auto original_size = pg_attribute_rows_.size();
    pg_attribute_rows_.erase(
        std::remove_if(pg_attribute_rows_.begin(), pg_attribute_rows_.end(),
                       [relid](const FormData_pg_attribute* r) { return r->attrelid == relid; }),
        pg_attribute_rows_.end());
    return original_size - pg_attribute_rows_.size();
}

// --- Catalog: pg_type ---

Oid Catalog::InsertType(FormData_pg_type* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertType: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    for (const auto* existing : pg_type_rows_) {
        if (existing->oid == row->oid) {
            ereport(error::LogLevel::kError, "Catalog::InsertType: duplicate OID");
        }
    }
    pg_type_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_type* Catalog::GetTypeByOid(Oid oid) const {
    for (const auto* row : pg_type_rows_) {
        if (row->oid == oid) {
            return row;
        }
    }
    return nullptr;
}

const FormData_pg_type* Catalog::GetTypeByName(const std::string& name) const {
    for (const auto* row : pg_type_rows_) {
        if (row->typname == name) {
            return row;
        }
    }
    return nullptr;
}

// --- Catalog: pg_operator ---

Oid Catalog::InsertOperator(FormData_pg_operator* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertOperator: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_operator_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_operator* Catalog::GetOperatorByOid(Oid oid) const {
    for (const auto* row : pg_operator_rows_) {
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_operator*> Catalog::GetOperatorsByName(
    const std::string& name) const {
    std::vector<const FormData_pg_operator*> result;
    for (const auto* row : pg_operator_rows_) {
        if (row->oprname == name)
            result.push_back(row);
    }
    return result;
}

const FormData_pg_operator* Catalog::GetOperator(const std::string& name, Oid left_type,
                                                 Oid right_type) const {
    for (const auto* row : pg_operator_rows_) {
        if (row->oprname == name && row->oprleft == left_type && row->oprright == right_type) {
            return row;
        }
    }
    return nullptr;
}

// --- Catalog: pg_proc ---

Oid Catalog::InsertProc(FormData_pg_proc* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertProc: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    // Compute pronargs from proargtypes if not set.
    if (row->pronargs == 0 && !row->proargtypes.empty()) {
        row->pronargs = static_cast<int16_t>(row->proargtypes.size());
    }
    pg_proc_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_proc* Catalog::GetProcByOid(Oid oid) const {
    for (const auto* row : pg_proc_rows_) {
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

std::vector<const FormData_pg_proc*> Catalog::GetProcsByName(const std::string& name) const {
    std::vector<const FormData_pg_proc*> result;
    for (const auto* row : pg_proc_rows_) {
        if (row->proname == name)
            result.push_back(row);
    }
    return result;
}

// --- Catalog: pg_cast ---

Oid Catalog::InsertCast(FormData_pg_cast* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertCast: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_cast_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_cast* Catalog::GetCast(Oid source_type, Oid target_type) const {
    for (const auto* row : pg_cast_rows_) {
        if (row->castsource == source_type && row->casttarget == target_type) {
            return row;
        }
    }
    return nullptr;
}

std::vector<const FormData_pg_cast*> Catalog::GetCastsBySource(Oid source_type) const {
    std::vector<const FormData_pg_cast*> result;
    for (const auto* row : pg_cast_rows_) {
        if (row->castsource == source_type)
            result.push_back(row);
    }
    return result;
}

// --- Catalog: pg_aggregate ---

void Catalog::InsertAggregate(FormData_pg_aggregate* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertAggregate: row is null");
    }
    pg_aggregate_rows_.push_back(row);
}

const FormData_pg_aggregate* Catalog::GetAggregate(Oid aggfnoid) const {
    for (const auto* row : pg_aggregate_rows_) {
        if (row->aggfnoid == aggfnoid)
            return row;
    }
    return nullptr;
}

// --- Catalog: pg_collation ---

Oid Catalog::InsertCollation(FormData_pg_collation* row) {
    if (row == nullptr) {
        ereport(error::LogLevel::kError, "Catalog::InsertCollation: row is null");
    }
    if (row->oid == kInvalidOid) {
        row->oid = AllocateOid();
    }
    pg_collation_rows_.push_back(row);
    return row->oid;
}

const FormData_pg_collation* Catalog::GetCollationByOid(Oid oid) const {
    for (const auto* row : pg_collation_rows_) {
        if (row->oid == oid)
            return row;
    }
    return nullptr;
}

const FormData_pg_collation* Catalog::GetCollationByName(const std::string& name) const {
    for (const auto* row : pg_collation_rows_) {
        if (row->collname == name)
            return row;
    }
    return nullptr;
}

// --- Catalog: OID assignment ---

Oid Catalog::AllocateOid() {
    return next_oid_++;
}

// --- Persistence helpers (A-3) ---
//
// Catalog rows are serialized to a simple TSV file. Only user-created rows
// (oid >= kFirstNormalObjectId, or for key-less tables, rows whose owning
// OID is in the user range) are persisted, on top of BootstrapCatalog's
// built-in rows. The format is:
//
//   PGCPP_CATALOG_V1
//   next_oid\t<N>
//   [pg_class]
//   <field>\t<field>\t...      (one row per line)
//   [pg_attribute]
//   ...
//
// Strings escape '\\', '\t', '\n', '\r' so every physical line is exactly one
// row or one section header. Missing file is not an error (fresh initdb).
namespace {

using pgcpp::nodes::makePallocNode;

constexpr const char* kCatalogMagic = "PGCPP_CATALOG_V1";

// Escape a string field so it cannot contain a raw tab or newline.
std::string EscapeStr(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c; break;
        }
    }
    return out;
}

std::string UnescapeStr(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            switch (s[i + 1]) {
                case '\\': out += '\\'; ++i; break;
                case 't':  out += '\t'; ++i; break;
                case 'n':  out += '\n'; ++i; break;
                case 'r':  out += '\r'; ++i; break;
                default:   out += s[i]; break;  // keep unknown escape as-is
            }
        } else {
            out += s[i];
        }
    }
    return out;
}

// Split a line on unescaped tabs; unescape each resulting field.
std::vector<std::string> SplitTab(const std::string& line) {
    std::vector<std::string> fields;
    std::string cur;
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            cur += line[i];
            cur += line[i + 1];
            ++i;
        } else if (line[i] == '\t') {
            fields.push_back(UnescapeStr(cur));
            cur.clear();
        } else {
            cur += line[i];
        }
    }
    fields.push_back(UnescapeStr(cur));
    return fields;
}

std::string JoinTab(const std::vector<std::string>& fields) {
    std::string out;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) out += '\t';
        out += fields[i];
    }
    return out;
}

// --- Numeric parsers (exception-free; AGENTS.md restricts exceptions to ereport) ---

bool ParseU32(const std::string& s, uint32_t& out) {
    errno = 0;
    char* end = nullptr;
    unsigned long v = std::strtoul(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    out = static_cast<uint32_t>(v);
    return true;
}

bool ParseI32(const std::string& s, int32_t& out) {
    errno = 0;
    char* end = nullptr;
    long v = std::strtol(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str() || *end != '\0') return false;
    out = static_cast<int32_t>(v);
    return true;
}

bool ParseI16(const std::string& s, int16_t& out) {
    int32_t v = 0;
    if (!ParseI32(s, v)) return false;
    out = static_cast<int16_t>(v);
    return true;
}

bool ParseFloat(const std::string& s, float& out) {
    errno = 0;
    char* end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (end == s.c_str() || *end != '\0') return false;
    out = v;
    return true;
}

// --- Field formatters (overload set) ---

std::string Fmt(bool b) { return b ? "1" : "0"; }
std::string Fmt(uint32_t v) { return std::to_string(v); }
std::string Fmt(int32_t v) { return std::to_string(v); }
std::string Fmt(int16_t v) { return std::to_string(v); }
std::string Fmt(float v) { return std::to_string(v); }
std::string Fmt(char c) { return std::string(1, c); }
std::string Fmt(const std::string& v) { return EscapeStr(v); }

template <typename E>
std::string FmtEnum(E e) {
    return std::string(1, static_cast<char>(e));
}

std::string FmtOidVec(const std::vector<Oid>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ',';
        out += std::to_string(v[i]);
    }
    return out;
}

std::string FmtStrVec(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ',';
        // Escape backslash and comma within elements.
        for (char c : v[i]) {
            if (c == '\\' || c == ',') out += '\\';
            out += c;
        }
    }
    return out;
}

std::vector<Oid> ParseOidVec(const std::string& s) {
    std::vector<Oid> out;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            uint32_t v = 0;
            if (ParseU32(cur, v)) out.push_back(v);
            cur.clear();
        }
    };
    for (char c : s) {
        if (c == ',') flush();
        else cur += c;
    }
    flush();
    return out;
}

std::vector<std::string> ParseStrVec(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            cur += s[i + 1];
            ++i;
        } else if (s[i] == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += s[i];
        }
    }
    out.push_back(cur);
    return out;
}

// --- Per-struct serializers (field order matches struct declaration order) ---

std::vector<std::string> Ser(const FormData_pg_class& r) {
    return {Fmt(r.oid),         Fmt(r.relname),        Fmt(r.relnamespace),
            Fmt(r.reltype),     Fmt(r.reloftype),      Fmt(r.relowner),
            Fmt(r.relam),       Fmt(r.relfilenode),    Fmt(r.reltablespace),
            Fmt(r.relpages),    Fmt(r.reltuples),      Fmt(r.reltoastrelid),
            Fmt(r.relhasindex), Fmt(r.relisshared),    FmtEnum(r.relpersistence),
            FmtEnum(r.relkind), Fmt(r.relnatts),       Fmt(r.relchecks),
            Fmt(r.relhasrules), Fmt(r.relhastriggers), Fmt(r.relrowsecurity),
            Fmt(r.relforcerowsecurity), Fmt(r.relispopulated),
            Fmt(r.relreplident), Fmt(r.relispartition), Fmt(r.relfrozenxid),
            Fmt(r.relminmxid)};
}

std::vector<std::string> Ser(const FormData_pg_attribute& r) {
    return {Fmt(r.attrelid),    Fmt(r.attname),      Fmt(r.atttypid),
            Fmt(r.attstattarget), Fmt(r.attlen),      Fmt(r.attnum),
            Fmt(r.attndims),    Fmt(r.attcacheoff),   Fmt(r.atttypmod),
            Fmt(r.attbyval),    FmtEnum(r.attstorage), FmtEnum(r.attalign),
            Fmt(r.attnotnull),  Fmt(r.atthasdef),     Fmt(r.atthasmissing),
            Fmt(r.attidentity), Fmt(r.attgenerated),  Fmt(r.attisdropped),
            Fmt(r.attislocal),  Fmt(r.attinhcount),   Fmt(r.attcollation)};
}

std::vector<std::string> Ser(const FormData_pg_type& r) {
    return {Fmt(r.oid),          Fmt(r.typname),         Fmt(r.typnamespace),
            Fmt(r.typowner),     Fmt(r.typlen),          Fmt(r.typbyval),
            FmtEnum(r.typtype),  FmtEnum(r.typcategory), Fmt(r.typispreferred),
            Fmt(r.typisdefined), Fmt(r.typdelim),        Fmt(r.typrelid),
            Fmt(r.typelem),      Fmt(r.typarray),        Fmt(r.typinput),
            Fmt(r.typoutput),    Fmt(r.typreceive),      Fmt(r.typsend),
            Fmt(r.typmodin),     Fmt(r.typmodout),       Fmt(r.typanalyze),
            FmtEnum(r.typalign), FmtEnum(r.typstorage),  Fmt(r.typnotnull),
            Fmt(r.typbasetype),  Fmt(r.typtypmod),       Fmt(r.typndims),
            Fmt(r.typcollation), Fmt(r.typdefault),      Fmt(r.typdefaultbin)};
}

std::vector<std::string> Ser(const FormData_pg_operator& r) {
    return {Fmt(r.oid),          Fmt(r.oprname),      Fmt(r.oprnamespace),
            Fmt(r.oprowner),     FmtEnum(r.oprkind),  Fmt(r.oprcanmerge),
            Fmt(r.oprcanhash),   Fmt(r.oprleft),      Fmt(r.oprright),
            Fmt(r.oprresult),    Fmt(r.oprcom),       Fmt(r.oprnegate),
            Fmt(r.oprcode),      Fmt(r.oprrest),      Fmt(r.oprjoin)};
}

std::vector<std::string> Ser(const FormData_pg_proc& r) {
    return {Fmt(r.oid),          Fmt(r.proname),         Fmt(r.pronamespace),
            Fmt(r.proowner),     Fmt(r.prolang),         Fmt(r.procost),
            Fmt(r.prorows),      Fmt(r.provariadic),     Fmt(r.prosupport),
            FmtEnum(r.prokind),  Fmt(r.prosecdef),       Fmt(r.proleakproof),
            Fmt(r.proisstrict),  Fmt(r.proretset),       FmtEnum(r.provolatile),
            FmtEnum(r.proparallel), Fmt(r.pronargs),     Fmt(r.pronargdefaults),
            Fmt(r.prorettype),   FmtOidVec(r.proargtypes),
            FmtOidVec(r.proallargtypes), Fmt(r.proargmodes),
            Fmt(r.proargnames),  Fmt(r.proargdefaults),  FmtOidVec(r.protrftypes),
            Fmt(r.prosrc),       Fmt(r.probin),          Fmt(r.prosqlbody),
            FmtStrVec(r.proconfig), FmtStrVec(r.proacl)};
}

std::vector<std::string> Ser(const FormData_pg_cast& r) {
    return {Fmt(r.oid), Fmt(r.castsource), Fmt(r.casttarget), Fmt(r.castfunc),
            FmtEnum(r.castcontext), FmtEnum(r.castmethod)};
}

std::vector<std::string> Ser(const FormData_pg_aggregate& r) {
    return {Fmt(r.aggfnoid),       FmtEnum(r.aggkind),        Fmt(r.aggnumdirectargs),
            Fmt(r.aggtransfn),     Fmt(r.aggfinalfn),         Fmt(r.aggcombinefn),
            Fmt(r.aggserialfn),    Fmt(r.aggdeserialfn),      Fmt(r.aggmtransfn),
            Fmt(r.aggminvtransfn), Fmt(r.aggmfinalfn),        Fmt(r.aggfinalextra),
            Fmt(r.aggmfinalextra), FmtEnum(r.aggfinalmodify), FmtEnum(r.aggmfinalmodify),
            Fmt(r.aggsortop),      Fmt(r.aggtranstype),       Fmt(r.aggtransspace),
            Fmt(r.aggmtranstype),  Fmt(r.aggmtransspace),     Fmt(r.agginitval),
            Fmt(r.aggminitval)};
}

std::vector<std::string> Ser(const FormData_pg_collation& r) {
    return {Fmt(r.oid),               Fmt(r.collname),          Fmt(r.collnamespace),
            Fmt(r.collowner),         FmtEnum(r.collprovider),  Fmt(r.collisdeterministic),
            Fmt(r.collencoding),      Fmt(r.collcollate),       Fmt(r.collctype),
            Fmt(r.colliculocale),     Fmt(r.collversion)};
}

// --- Per-struct deserializers ---
//
// Each parses into a stack temporary; on any parse failure it returns
// nullptr (Load skips the row). On success it palloc-allocates a row via
// copy construction so std::string/std::vector members are registered for
// destruction with the owning MemoryContext.

FormData_pg_class* DeserPgClass(const std::vector<std::string>& f) {
    if (f.size() < 27) return nullptr;
    FormData_pg_class t;
    if (!ParseU32(f[0], t.oid)) return nullptr;
    t.relname = f[1];
    if (!ParseU32(f[2], t.relnamespace)) return nullptr;
    if (!ParseU32(f[3], t.reltype)) return nullptr;
    if (!ParseU32(f[4], t.reloftype)) return nullptr;
    if (!ParseU32(f[5], t.relowner)) return nullptr;
    if (!ParseU32(f[6], t.relam)) return nullptr;
    if (!ParseU32(f[7], t.relfilenode)) return nullptr;
    if (!ParseU32(f[8], t.reltablespace)) return nullptr;
    if (!ParseI32(f[9], t.relpages)) return nullptr;
    if (!ParseFloat(f[10], t.reltuples)) return nullptr;
    if (!ParseU32(f[11], t.reltoastrelid)) return nullptr;
    if (f[12] != "0" && f[12] != "1") return nullptr;
    t.relhasindex = f[12] == "1";
    if (f[13] != "0" && f[13] != "1") return nullptr;
    t.relisshared = f[13] == "1";
    if (f[14].empty()) return nullptr;
    t.relpersistence = static_cast<RelPersistence>(f[14][0]);
    if (f[15].empty()) return nullptr;
    t.relkind = static_cast<RelKind>(f[15][0]);
    if (!ParseI16(f[16], t.relnatts)) return nullptr;
    if (!ParseI16(f[17], t.relchecks)) return nullptr;
    if (f[18] != "0" && f[18] != "1") return nullptr;
    t.relhasrules = f[18] == "1";
    if (f[19] != "0" && f[19] != "1") return nullptr;
    t.relhastriggers = f[19] == "1";
    if (f[20] != "0" && f[20] != "1") return nullptr;
    t.relrowsecurity = f[20] == "1";
    if (f[21] != "0" && f[21] != "1") return nullptr;
    t.relforcerowsecurity = f[21] == "1";
    if (f[22] != "0" && f[22] != "1") return nullptr;
    t.relispopulated = f[22] == "1";
    if (f[23].empty()) return nullptr;
    t.relreplident = f[23][0];
    if (f[24] != "0" && f[24] != "1") return nullptr;
    t.relispartition = f[24] == "1";
    if (!ParseU32(f[25], t.relfrozenxid)) return nullptr;
    if (!ParseU32(f[26], t.relminmxid)) return nullptr;
    return makePallocNode<FormData_pg_class>(t);
}

FormData_pg_attribute* DeserPgAttribute(const std::vector<std::string>& f) {
    if (f.size() < 21) return nullptr;
    FormData_pg_attribute t;
    if (!ParseU32(f[0], t.attrelid)) return nullptr;
    t.attname = f[1];
    if (!ParseU32(f[2], t.atttypid)) return nullptr;
    if (!ParseI32(f[3], t.attstattarget)) return nullptr;
    if (!ParseI16(f[4], t.attlen)) return nullptr;
    if (!ParseI16(f[5], t.attnum)) return nullptr;
    if (!ParseI16(f[6], t.attndims)) return nullptr;
    if (!ParseI32(f[7], t.attcacheoff)) return nullptr;
    if (!ParseI32(f[8], t.atttypmod)) return nullptr;
    if (f[9] != "0" && f[9] != "1") return nullptr;
    t.attbyval = f[9] == "1";
    if (f[10].empty()) return nullptr;
    t.attstorage = static_cast<AttStorage>(f[10][0]);
    if (f[11].empty()) return nullptr;
    t.attalign = static_cast<AttAlign>(f[11][0]);
    if (f[12] != "0" && f[12] != "1") return nullptr;
    t.attnotnull = f[12] == "1";
    if (f[13] != "0" && f[13] != "1") return nullptr;
    t.atthasdef = f[13] == "1";
    if (f[14] != "0" && f[14] != "1") return nullptr;
    t.atthasmissing = f[14] == "1";
    t.attidentity = f[15].empty() ? '\0' : f[15][0];
    t.attgenerated = f[16].empty() ? '\0' : f[16][0];
    if (f[17] != "0" && f[17] != "1") return nullptr;
    t.attisdropped = f[17] == "1";
    if (f[18] != "0" && f[18] != "1") return nullptr;
    t.attislocal = f[18] == "1";
    if (!ParseI16(f[19], t.attinhcount)) return nullptr;
    if (!ParseU32(f[20], t.attcollation)) return nullptr;
    return makePallocNode<FormData_pg_attribute>(t);
}

FormData_pg_type* DeserPgType(const std::vector<std::string>& f) {
    if (f.size() < 30) return nullptr;
    FormData_pg_type t;
    if (!ParseU32(f[0], t.oid)) return nullptr;
    t.typname = f[1];
    if (!ParseU32(f[2], t.typnamespace)) return nullptr;
    if (!ParseU32(f[3], t.typowner)) return nullptr;
    if (!ParseI16(f[4], t.typlen)) return nullptr;
    if (f[5] != "0" && f[5] != "1") return nullptr;
    t.typbyval = f[5] == "1";
    if (f[6].empty()) return nullptr;
    t.typtype = static_cast<TypeType>(f[6][0]);
    if (f[7].empty()) return nullptr;
    t.typcategory = static_cast<TypeCategory>(f[7][0]);
    if (f[8] != "0" && f[8] != "1") return nullptr;
    t.typispreferred = f[8] == "1";
    if (f[9] != "0" && f[9] != "1") return nullptr;
    t.typisdefined = f[9] == "1";
    t.typdelim = f[10].empty() ? ',' : f[10][0];
    if (!ParseU32(f[11], t.typrelid)) return nullptr;
    if (!ParseU32(f[12], t.typelem)) return nullptr;
    if (!ParseU32(f[13], t.typarray)) return nullptr;
    if (!ParseU32(f[14], t.typinput)) return nullptr;
    if (!ParseU32(f[15], t.typoutput)) return nullptr;
    if (!ParseU32(f[16], t.typreceive)) return nullptr;
    if (!ParseU32(f[17], t.typsend)) return nullptr;
    if (!ParseU32(f[18], t.typmodin)) return nullptr;
    if (!ParseU32(f[19], t.typmodout)) return nullptr;
    if (!ParseU32(f[20], t.typanalyze)) return nullptr;
    if (f[21].empty()) return nullptr;
    t.typalign = static_cast<TypeAlign>(f[21][0]);
    if (f[22].empty()) return nullptr;
    t.typstorage = static_cast<TypeStorage>(f[22][0]);
    if (f[23] != "0" && f[23] != "1") return nullptr;
    t.typnotnull = f[23] == "1";
    if (!ParseU32(f[24], t.typbasetype)) return nullptr;
    if (!ParseI32(f[25], t.typtypmod)) return nullptr;
    if (!ParseI32(f[26], t.typndims)) return nullptr;
    if (!ParseU32(f[27], t.typcollation)) return nullptr;
    t.typdefault = f[28];
    t.typdefaultbin = f[29];
    return makePallocNode<FormData_pg_type>(t);
}

FormData_pg_operator* DeserPgOperator(const std::vector<std::string>& f) {
    if (f.size() < 15) return nullptr;
    FormData_pg_operator t;
    if (!ParseU32(f[0], t.oid)) return nullptr;
    t.oprname = f[1];
    if (!ParseU32(f[2], t.oprnamespace)) return nullptr;
    if (!ParseU32(f[3], t.oprowner)) return nullptr;
    if (f[4].empty()) return nullptr;
    t.oprkind = static_cast<OperatorKind>(f[4][0]);
    if (f[5] != "0" && f[5] != "1") return nullptr;
    t.oprcanmerge = f[5] == "1";
    if (f[6] != "0" && f[6] != "1") return nullptr;
    t.oprcanhash = f[6] == "1";
    if (!ParseU32(f[7], t.oprleft)) return nullptr;
    if (!ParseU32(f[8], t.oprright)) return nullptr;
    if (!ParseU32(f[9], t.oprresult)) return nullptr;
    if (!ParseU32(f[10], t.oprcom)) return nullptr;
    if (!ParseU32(f[11], t.oprnegate)) return nullptr;
    if (!ParseU32(f[12], t.oprcode)) return nullptr;
    if (!ParseU32(f[13], t.oprrest)) return nullptr;
    if (!ParseU32(f[14], t.oprjoin)) return nullptr;
    return makePallocNode<FormData_pg_operator>(t);
}

FormData_pg_proc* DeserPgProc(const std::vector<std::string>& f) {
    if (f.size() < 30) return nullptr;
    FormData_pg_proc t;
    if (!ParseU32(f[0], t.oid)) return nullptr;
    t.proname = f[1];
    if (!ParseU32(f[2], t.pronamespace)) return nullptr;
    if (!ParseU32(f[3], t.proowner)) return nullptr;
    if (!ParseU32(f[4], t.prolang)) return nullptr;
    if (!ParseFloat(f[5], t.procost)) return nullptr;
    if (!ParseFloat(f[6], t.prorows)) return nullptr;
    if (!ParseU32(f[7], t.provariadic)) return nullptr;
    if (!ParseU32(f[8], t.prosupport)) return nullptr;
    if (f[9].empty()) return nullptr;
    t.prokind = static_cast<ProKind>(f[9][0]);
    if (f[10] != "0" && f[10] != "1") return nullptr;
    t.prosecdef = f[10] == "1";
    if (f[11] != "0" && f[11] != "1") return nullptr;
    t.proleakproof = f[11] == "1";
    if (f[12] != "0" && f[12] != "1") return nullptr;
    t.proisstrict = f[12] == "1";
    if (f[13] != "0" && f[13] != "1") return nullptr;
    t.proretset = f[13] == "1";
    if (f[14].empty()) return nullptr;
    t.provolatile = static_cast<ProVolatile>(f[14][0]);
    if (f[15].empty()) return nullptr;
    t.proparallel = static_cast<ProParallel>(f[15][0]);
    if (!ParseI16(f[16], t.pronargs)) return nullptr;
    if (!ParseI16(f[17], t.pronargdefaults)) return nullptr;
    if (!ParseU32(f[18], t.prorettype)) return nullptr;
    t.proargtypes = ParseOidVec(f[19]);
    t.proallargtypes = ParseOidVec(f[20]);
    t.proargmodes = f[21];
    t.proargnames = f[22];
    t.proargdefaults = f[23];
    t.protrftypes = ParseOidVec(f[24]);
    t.prosrc = f[25];
    t.probin = f[26];
    t.prosqlbody = f[27];
    t.proconfig = ParseStrVec(f[28]);
    t.proacl = ParseStrVec(f[29]);
    return makePallocNode<FormData_pg_proc>(t);
}

FormData_pg_cast* DeserPgCast(const std::vector<std::string>& f) {
    if (f.size() < 6) return nullptr;
    FormData_pg_cast t;
    if (!ParseU32(f[0], t.oid)) return nullptr;
    if (!ParseU32(f[1], t.castsource)) return nullptr;
    if (!ParseU32(f[2], t.casttarget)) return nullptr;
    if (!ParseU32(f[3], t.castfunc)) return nullptr;
    if (f[4].empty()) return nullptr;
    t.castcontext = static_cast<CastContext>(f[4][0]);
    if (f[5].empty()) return nullptr;
    t.castmethod = static_cast<CastMethod>(f[5][0]);
    return makePallocNode<FormData_pg_cast>(t);
}

FormData_pg_aggregate* DeserPgAggregate(const std::vector<std::string>& f) {
    if (f.size() < 22) return nullptr;
    FormData_pg_aggregate t;
    if (!ParseU32(f[0], t.aggfnoid)) return nullptr;
    if (f[1].empty()) return nullptr;
    t.aggkind = static_cast<AggKind>(f[1][0]);
    if (!ParseI16(f[2], t.aggnumdirectargs)) return nullptr;
    if (!ParseU32(f[3], t.aggtransfn)) return nullptr;
    if (!ParseU32(f[4], t.aggfinalfn)) return nullptr;
    if (!ParseU32(f[5], t.aggcombinefn)) return nullptr;
    if (!ParseU32(f[6], t.aggserialfn)) return nullptr;
    if (!ParseU32(f[7], t.aggdeserialfn)) return nullptr;
    if (!ParseU32(f[8], t.aggmtransfn)) return nullptr;
    if (!ParseU32(f[9], t.aggminvtransfn)) return nullptr;
    if (!ParseU32(f[10], t.aggmfinalfn)) return nullptr;
    if (f[11] != "0" && f[11] != "1") return nullptr;
    t.aggfinalextra = f[11] == "1";
    if (f[12] != "0" && f[12] != "1") return nullptr;
    t.aggmfinalextra = f[12] == "1";
    if (f[13].empty()) return nullptr;
    t.aggfinalmodify = static_cast<AggModify>(f[13][0]);
    if (f[14].empty()) return nullptr;
    t.aggmfinalmodify = static_cast<AggModify>(f[14][0]);
    if (!ParseU32(f[15], t.aggsortop)) return nullptr;
    if (!ParseU32(f[16], t.aggtranstype)) return nullptr;
    if (!ParseI32(f[17], t.aggtransspace)) return nullptr;
    if (!ParseU32(f[18], t.aggmtranstype)) return nullptr;
    if (!ParseI32(f[19], t.aggmtransspace)) return nullptr;
    t.agginitval = f[20];
    t.aggminitval = f[21];
    return makePallocNode<FormData_pg_aggregate>(t);
}

FormData_pg_collation* DeserPgCollation(const std::vector<std::string>& f) {
    if (f.size() < 11) return nullptr;
    FormData_pg_collation t;
    if (!ParseU32(f[0], t.oid)) return nullptr;
    t.collname = f[1];
    if (!ParseU32(f[2], t.collnamespace)) return nullptr;
    if (!ParseU32(f[3], t.collowner)) return nullptr;
    if (f[4].empty()) return nullptr;
    t.collprovider = static_cast<CollProvider>(f[4][0]);
    if (f[5] != "0" && f[5] != "1") return nullptr;
    t.collisdeterministic = f[5] == "1";
    if (!ParseI32(f[6], t.collencoding)) return nullptr;
    t.collcollate = f[7];
    t.collctype = f[8];
    t.colliculocale = f[9];
    t.collversion = f[10];
    return makePallocNode<FormData_pg_collation>(t);
}

}  // namespace

// --- Catalog: persistence (A-3) ---

bool Catalog::Save(const std::string& path) const {
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open()) return false;

    out << kCatalogMagic << '\n';
    out << "next_oid\t" << next_oid_ << '\n';

    auto write_section = [&](const char* name, const std::string& body) {
        out << name << '\n' << body;
    };

    auto emit_class = [&]() {
        std::string body;
        for (const auto* r : pg_class_rows_) {
            if (r->oid < kFirstNormalObjectId) continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_attr = [&]() {
        std::string body;
        for (const auto* r : pg_attribute_rows_) {
            if (r->attrelid < kFirstNormalObjectId) continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_type = [&]() {
        std::string body;
        for (const auto* r : pg_type_rows_) {
            if (r->oid < kFirstNormalObjectId) continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_op = [&]() {
        std::string body;
        for (const auto* r : pg_operator_rows_) {
            if (r->oid < kFirstNormalObjectId) continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_proc = [&]() {
        std::string body;
        for (const auto* r : pg_proc_rows_) {
            if (r->oid < kFirstNormalObjectId) continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_cast = [&]() {
        std::string body;
        for (const auto* r : pg_cast_rows_) {
            if (r->oid < kFirstNormalObjectId) continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_agg = [&]() {
        std::string body;
        for (const auto* r : pg_aggregate_rows_) {
            if (r->aggfnoid < kFirstNormalObjectId) continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };
    auto emit_coll = [&]() {
        std::string body;
        for (const auto* r : pg_collation_rows_) {
            if (r->oid < kFirstNormalObjectId) continue;
            body += JoinTab(Ser(*r)) + '\n';
        }
        return body;
    };

    write_section("[pg_class]", emit_class());
    write_section("[pg_attribute]", emit_attr());
    write_section("[pg_type]", emit_type());
    write_section("[pg_operator]", emit_op());
    write_section("[pg_proc]", emit_proc());
    write_section("[pg_cast]", emit_cast());
    write_section("[pg_aggregate]", emit_agg());
    write_section("[pg_collation]", emit_coll());

    out.flush();
    return out.good();
}

bool Catalog::Load(const std::string& path) {
    std::ifstream in(path, std::ios::in);
    if (!in.is_open()) return false;  // missing file = fresh initdb

    std::string line;
    if (!std::getline(in, line) || line != kCatalogMagic) return false;

    Oid saved_next_oid = kFirstNormalObjectId;
    if (!std::getline(in, line)) return false;
    {
        auto fields = SplitTab(line);
        if (fields.size() < 2 || fields[0] != "next_oid" || !ParseU32(fields[1], saved_next_oid))
            return false;
    }

    // Read rows grouped under their section header. Rows carry their own OID
    // (>= kFirstNormalObjectId), so the Insert* methods preserve it without
    // calling AllocateOid; next_oid_ is restored once at the end.
    std::string section;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == '[') {
            section = line;
            continue;
        }
        auto fields = SplitTab(line);
        if (fields.empty()) continue;

        if (section == "[pg_class]") {
            if (auto* r = DeserPgClass(fields)) InsertClass(r);
        } else if (section == "[pg_attribute]") {
            if (auto* r = DeserPgAttribute(fields)) InsertAttribute(r);
        } else if (section == "[pg_type]") {
            if (auto* r = DeserPgType(fields)) InsertType(r);
        } else if (section == "[pg_operator]") {
            if (auto* r = DeserPgOperator(fields)) InsertOperator(r);
        } else if (section == "[pg_proc]") {
            if (auto* r = DeserPgProc(fields)) InsertProc(r);
        } else if (section == "[pg_cast]") {
            if (auto* r = DeserPgCast(fields)) InsertCast(r);
        } else if (section == "[pg_aggregate]") {
            if (auto* r = DeserPgAggregate(fields)) InsertAggregate(r);
        } else if (section == "[pg_collation]") {
            if (auto* r = DeserPgCollation(fields)) InsertCollation(r);
        }
    }

    SetNextOid(saved_next_oid);
    return true;
}

// --- Global accessors ---

Catalog* GetCatalog() {
    return g_catalog;
}

void SetCatalog(Catalog* catalog) {
    g_catalog = catalog;
}

// --- CatalogTuple* API ---

Oid CatalogTupleInsert(FormData_pg_class* row) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(error::LogLevel::kError, "CatalogTupleInsert: catalog not initialized");
    }
    return cat->InsertClass(row);
}

bool CatalogTupleUpdate(Oid oid, const FormData_pg_class* new_row) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(error::LogLevel::kError, "CatalogTupleUpdate: catalog not initialized");
    }
    return cat->UpdateClass(oid, new_row);
}

bool CatalogTupleDelete(Oid oid) {
    Catalog* cat = GetCatalog();
    if (cat == nullptr) {
        ereport(error::LogLevel::kError, "CatalogTupleDelete: catalog not initialized");
    }
    return cat->DeleteClass(oid);
}

}  // namespace pgcpp::catalog
