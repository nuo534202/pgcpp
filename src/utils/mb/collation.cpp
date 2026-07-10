// collation.cpp — collation-aware string comparison.
//
// Converted from PostgreSQL 15's src/backend/utils/adt/varlena.c (varstr_cmp).

#include "utils/mb/collation.hpp"

#include <cstring>
#include <vector>

#include "catalog/catalog.hpp"
#include "catalog/pg_collation.hpp"
#include "utils/mb/mbutils.hpp"
#include "utils/mb/wchar.hpp"

namespace pgcpp::utils {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::CollProvider;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::kC_COLLATION_OID;
using pgcpp::catalog::kDefaultCollationOid;
using pgcpp::catalog::kInvalidOid;
using pgcpp::catalog::kPOSIX_COLLATION_OID;
using pgcpp::catalog::Oid;

int VarStrCmpC(std::string_view a, std::string_view b) {
    int cmp = std::memcmp(a.data(), b.data(), std::min(a.size(), b.size()));
    if (cmp != 0)
        return cmp;
    // If the common prefix matches, the shorter string sorts first.
    if (a.size() < b.size())
        return -1;
    if (a.size() > b.size())
        return 1;
    return 0;
}

int VarStrCmpCodepoint(std::string_view a, std::string_view b, PgEncoding enc) {
    if (enc != PgEncoding::kUtf8) {
        // For non-UTF-8, byte comparison is the same as codepoint comparison
        // (each byte is one codepoint for single-byte encodings).
        return VarStrCmpC(a, b);
    }

    // UTF-8: decode both strings to codepoints and compare.
    std::vector<uint32_t> wa, wb;
    PgMb2wchar(PgEncoding::kUtf8, a, &wa);
    PgMb2wchar(PgEncoding::kUtf8, b, &wb);

    size_t min_len = std::min(wa.size(), wb.size());
    for (size_t i = 0; i < min_len; i++) {
        if (wa[i] < wb[i])
            return -1;
        if (wa[i] > wb[i])
            return 1;
    }
    if (wa.size() < wb.size())
        return -1;
    if (wa.size() > wb.size())
        return 1;
    return 0;
}

CollProvider GetCollationProvider(Oid collation_oid) {
    if (collation_oid == kInvalidOid || collation_oid == kDefaultCollationOid)
        return CollProvider::kDefault;

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return CollProvider::kDefault;

    const auto* coll = cat->GetCollationByOid(collation_oid);
    if (coll == nullptr)
        return CollProvider::kDefault;

    return coll->collprovider;
}

int VarStrCmp(std::string_view a, std::string_view b, Oid collation_oid) {
    // C and POSIX collations: pure byte comparison.
    if (collation_oid == kC_COLLATION_OID || collation_oid == kPOSIX_COLLATION_OID ||
        collation_oid == kInvalidOid || collation_oid == kDefaultCollationOid) {
        return VarStrCmpC(a, b);
    }

    // Look up the collation provider.
    CollProvider provider = GetCollationProvider(collation_oid);

    switch (provider) {
        case CollProvider::kDefault:
        case CollProvider::kLibc:
            // Without locale-aware strcoll, fall back to byte comparison.
            return VarStrCmpC(a, b);
        case CollProvider::kIcu:
            // ICU collation: use Unicode codepoint comparison as a
            // simplified substitute (full ICU is not linked).
            return VarStrCmpCodepoint(a, b, GetDatabaseEncoding());
    }

    return VarStrCmpC(a, b);
}

}  // namespace pgcpp::utils
