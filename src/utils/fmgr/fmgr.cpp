// fmgr.cpp — Function manager implementation.
//
// Converted from PostgreSQL 15's src/backend/utils/fmgr/fmgr.c.
//
// The function manager provides a uniform calling convention for all
// PostgreSQL functions. This file implements:
//   - A builtin function lookup table mapping pg_proc OIDs to C function
//     pointers (wrappers adapting the builtin Datum signatures to the
//     PgFunction / FunctionCallInfo convention).
//   - fmgr_info(): look up a function by OID and fill FmgrInfo.
//   - FunctionCall(): dispatch a call via FmgrInfo, with strict-NULL
//     short-circuiting.
//   - DirectFunctionCallN / OidFunctionCallN convenience wrappers.
#include "utils/fmgr.hpp"

#include <algorithm>
#include <cmath>

#include "catalog/catalog.hpp"
#include "catalog/pg_proc.hpp"
#include "pl/pl_handler.hpp"
#include "types/builtins.hpp"
#include "types/math_funcs.hpp"
#include "types/string_funcs.hpp"

namespace pgcpp::fmgr {

using pgcpp::catalog::Catalog;
using pgcpp::catalog::FormData_pg_proc;
using pgcpp::catalog::GetCatalog;
using pgcpp::catalog::Oid;
using pgcpp::types::Datum;

// --- Builtin function wrappers ---
//
// Each wrapper extracts arguments from FunctionCallInfo, calls the actual
// builtin function (which uses the simpler Datum-by-value convention), sets
// the result, and returns it. This mirrors PostgreSQL's fmgr_builtins table
// where every entry is a PG_FUNCTION_INFO_V1-decorated C function.

static Datum fmgr_int4_abs(FunctionCallInfo& fc) {
    return pgcpp::types::int4_abs(fc.arg[0]);
}

static Datum fmgr_int4_mod(FunctionCallInfo& fc) {
    return pgcpp::types::int4_mod(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_float8_round(FunctionCallInfo& fc) {
    return pgcpp::types::float8_round(fc.arg[0]);
}

static Datum fmgr_float8_ceil(FunctionCallInfo& fc) {
    return pgcpp::types::float8_ceil(fc.arg[0]);
}

static Datum fmgr_float8_floor(FunctionCallInfo& fc) {
    return pgcpp::types::float8_floor(fc.arg[0]);
}

static Datum fmgr_float8_pl(FunctionCallInfo& fc) {
    return pgcpp::types::float8_pl(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_float8_mi(FunctionCallInfo& fc) {
    return pgcpp::types::float8_mi(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_float8_mul(FunctionCallInfo& fc) {
    return pgcpp::types::float8_mul(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_float8_div(FunctionCallInfo& fc) {
    return pgcpp::types::float8_div(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int4_pl(FunctionCallInfo& fc) {
    return pgcpp::types::int4_pl(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int4_mi(FunctionCallInfo& fc) {
    return pgcpp::types::int4_mi(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int4_mul(FunctionCallInfo& fc) {
    return pgcpp::types::int4_mul(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int4_div(FunctionCallInfo& fc) {
    return pgcpp::types::int4_div(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int8_pl(FunctionCallInfo& fc) {
    return pgcpp::types::int8_pl(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int8_mi(FunctionCallInfo& fc) {
    return pgcpp::types::int8_mi(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int8_mul(FunctionCallInfo& fc) {
    return pgcpp::types::int8_mul(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_text_length(FunctionCallInfo& fc) {
    return pgcpp::types::text_length(fc.arg[0]);
}

static Datum fmgr_text_eq(FunctionCallInfo& fc) {
    return pgcpp::types::text_eq(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_text_concat(FunctionCallInfo& fc) {
    return pgcpp::types::text_concat(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int4_eq(FunctionCallInfo& fc) {
    return pgcpp::types::int4_eq(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int4_lt(FunctionCallInfo& fc) {
    return pgcpp::types::int4_lt(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int4_gt(FunctionCallInfo& fc) {
    return pgcpp::types::int4_gt(fc.arg[0], fc.arg[1]);
}

// --- Task 9: math function wrappers ---

static Datum fmgr_int8_abs(FunctionCallInfo& fc) {
    return pgcpp::types::int8_abs(fc.arg[0]);
}

static Datum fmgr_float8_abs(FunctionCallInfo& fc) {
    return pgcpp::types::float8_abs(fc.arg[0]);
}

static Datum fmgr_float8_sqrt(FunctionCallInfo& fc) {
    return pgcpp::types::Float8GetDatum(std::sqrt(pgcpp::types::DatumGetFloat8(fc.arg[0])));
}

static Datum fmgr_float8_power(FunctionCallInfo& fc) {
    return pgcpp::types::Float8GetDatum(
        std::pow(pgcpp::types::DatumGetFloat8(fc.arg[0]), pgcpp::types::DatumGetFloat8(fc.arg[1])));
}

static Datum fmgr_float8_ln(FunctionCallInfo& fc) {
    return pgcpp::types::float8_ln(fc.arg[0]);
}

static Datum fmgr_float8_log10(FunctionCallInfo& fc) {
    return pgcpp::types::float8_log10(fc.arg[0]);
}

static Datum fmgr_float8_exp(FunctionCallInfo& fc) {
    return pgcpp::types::float8_exp(fc.arg[0]);
}

static Datum fmgr_float8_trunc(FunctionCallInfo& fc) {
    return pgcpp::types::float8_trunc(fc.arg[0]);
}

static Datum fmgr_int8_mod(FunctionCallInfo& fc) {
    return pgcpp::types::int8_mod(fc.arg[0], fc.arg[1]);
}

static Datum fmgr_int4_sign(FunctionCallInfo& fc) {
    return pgcpp::types::int4_sign(fc.arg[0]);
}

// --- Builtin function table ---
//
// Maps pg_proc OIDs to their C function wrappers. The OIDs must match the
// values used in bootstrap_catalog.cpp's InsertProc calls.

struct BuiltinEntry {
    Oid oid;
    PgFunction func;
    const char* proname;
};

static const BuiltinEntry kBuiltinTable[] = {
    // Arithmetic — int4
    {551, fmgr_int4_pl, "int4pl"},
    {550, fmgr_int4_mi, "int4mi"},
    {552, fmgr_int4_mul, "int4mul"},
    {553, fmgr_int4_div, "int4div"},
    // Arithmetic — int8
    {604, fmgr_int8_pl, "int8pl"},
    {605, fmgr_int8_mi, "int8mi"},
    {606, fmgr_int8_mul, "int8mul"},
    // Arithmetic — float8
    {612, fmgr_float8_pl, "float8pl"},
    {613, fmgr_float8_mi, "float8mi"},
    {614, fmgr_float8_mul, "float8mul"},
    {615, fmgr_float8_div, "float8div"},
    // Math functions — abs / round / ceil / floor
    {1398, fmgr_int4_abs, "abs"},
    {1796, fmgr_int8_abs, "abs"},
    {1346, fmgr_float8_abs, "abs"},
    {1700, fmgr_float8_round, "round"},
    {2308, fmgr_float8_ceil, "ceil"},
    {2311, fmgr_float8_floor, "floor"},
    // Math functions — sqrt / power / log / log10 / exp / sign / trunc
    {1340, fmgr_float8_sqrt, "sqrt"},
    {1368, fmgr_float8_power, "power"},
    {1342, fmgr_float8_ln, "log"},
    {1343, fmgr_float8_log10, "log10"},
    {1341, fmgr_float8_exp, "exp"},
    {1345, fmgr_int4_sign, "sign"},
    {1344, fmgr_float8_trunc, "trunc"},
    // Math functions — mod (int4 / int8)
    {941, fmgr_int4_mod, "mod"},
    {947, fmgr_int8_mod, "mod"},
    // Comparison
    {350, fmgr_int4_eq, "int4eq"},
    {97, fmgr_int4_lt, "int4lt"},
    {521, fmgr_int4_gt, "int4gt"},
    // Text functions
    {1311, fmgr_text_length, "length"},
    {98, fmgr_text_eq, "texteq"},
    {3665, fmgr_text_concat, "concat"},
};

static constexpr int kBuiltinTableSize = sizeof(kBuiltinTable) / sizeof(kBuiltinTable[0]);

// Lookup a PgFunction by OID in the builtin table.
static PgFunction LookupBuiltinByOid(Oid oid) {
    for (int i = 0; i < kBuiltinTableSize; ++i) {
        if (kBuiltinTable[i].oid == oid)
            return kBuiltinTable[i].func;
    }
    return nullptr;
}

PgFunction LookupBuiltinFunction(const std::string& proname) {
    // Find the first builtin table entry whose proname matches.
    for (int i = 0; i < kBuiltinTableSize; ++i) {
        if (kBuiltinTable[i].proname == proname)
            return kBuiltinTable[i].func;
    }
    return nullptr;
}

// --- fmgr_info ---

bool fmgr_info(Oid funcid, FmgrInfo* finfo) {
    if (finfo == nullptr)
        return false;

    // Clear the struct.
    finfo->fn_oid = 0;
    finfo->fn_addr = nullptr;
    finfo->fn_language = 0;
    finfo->fn_strict = true;
    finfo->fn_name.clear();
    finfo->fn_pl_handler = nullptr;

    Catalog* cat = GetCatalog();
    if (cat == nullptr)
        return false;

    const FormData_pg_proc* proc = cat->GetProcByOid(funcid);
    if (proc == nullptr)
        return false;

    finfo->fn_oid = funcid;
    finfo->fn_name = proc->proname;
    finfo->fn_strict = proc->proisstrict;

    // Determine language. Builtins in bootstrap_catalog don't set prolang
    // (it defaults to kInvalidOid); treat that as internal language.
    Oid lang = proc->prolang;
    if (lang == pgcpp::catalog::kInvalidOid)
        lang = kInternalLanguageOid;
    finfo->fn_language = lang;

    if (lang == kInternalLanguageOid) {
        // Internal language: look up the C function pointer by OID.
        finfo->fn_addr = LookupBuiltinByOid(funcid);
    } else if (lang == kCLanguageOid) {
        // C language: look up by prosrc (the C symbol name) in the builtin
        // table. For user-defined C functions (dynamically loaded), fn_addr
        // remains nullptr — pgcpp does not yet support dynamic loading.
        if (!proc->prosrc.empty())
            finfo->fn_addr = LookupBuiltinFunction(proc->prosrc);
    } else if (lang == kSqlLanguageOid) {
        // SQL language: no C handler; execution is via SQL parsing.
        finfo->fn_addr = nullptr;
    } else {
        // Procedural language (plpgsql, etc.): look up a registered PL
        // handler by language OID. fn_addr remains nullptr; FunctionCall
        // dispatches to fn_pl_handler->call_cb when invoked.
        finfo->fn_pl_handler = pgcpp::pl::LookupPlHandler(lang);
    }

    // fn_addr may be nullptr for SQL functions or unregistered builtins;
    // fmgr_info still succeeds (the metadata is valid), but FunctionCall
    // will need to handle the SQL case separately.
    return true;
}

// --- FunctionCall ---

Datum FunctionCall(const FmgrInfo* finfo, const std::vector<Datum>& args, bool* isnull) {
    if (isnull != nullptr)
        *isnull = false;

    if (finfo == nullptr) {
        if (isnull != nullptr)
            *isnull = true;
        return 0;
    }

    // Note: in this non-nulls variant, all args are non-NULL by convention,
    // so the strict-NULL check is not applicable (see FunctionCallWithNulls).

    FunctionCallInfo fc;
    fc.flinfo = finfo;
    fc.nargs = static_cast<int>(std::min(args.size(), size_t(10)));
    for (int i = 0; i < fc.nargs; ++i)
        fc.arg[i] = args[i];

    if (finfo->fn_addr != nullptr) {
        fc.result = finfo->fn_addr(fc);
        fc.isnull_result = false;
        return fc.result;
    }

    // Procedural language function: dispatch to the PL handler.
    if (finfo->fn_pl_handler != nullptr && finfo->fn_pl_handler->call_cb != nullptr) {
        fc.result = finfo->fn_pl_handler->call_cb(fc);
        if (fc.isnull_result && isnull != nullptr)
            *isnull = true;
        return fc.result;
    }

    // SQL-language functions: pgcpp does not yet execute SQL function bodies
    // through fmgr. Return NULL for now.
    if (isnull != nullptr)
        *isnull = true;
    return 0;
}

Datum FunctionCallWithNulls(const FmgrInfo* finfo, const std::vector<Datum>& args,
                            const std::vector<bool>& isnulls, bool* isnull) {
    if (isnull != nullptr)
        *isnull = false;

    if (finfo == nullptr) {
        if (isnull != nullptr)
            *isnull = true;
        return 0;
    }

    // Strict function: if any argument is NULL, return NULL immediately.
    if (finfo->fn_strict) {
        int n = static_cast<int>(std::min(isnulls.size(), size_t(10)));
        for (int i = 0; i < n; ++i) {
            if (isnulls[i]) {
                if (isnull != nullptr)
                    *isnull = true;
                return 0;
            }
        }
    }

    FunctionCallInfo fc;
    fc.flinfo = finfo;
    fc.nargs = static_cast<int>(std::min(args.size(), size_t(10)));
    for (int i = 0; i < fc.nargs; ++i) {
        fc.arg[i] = args[i];
        fc.isnull[i] = (i < static_cast<int>(isnulls.size())) ? isnulls[i] : false;
    }

    if (finfo->fn_addr != nullptr) {
        fc.result = finfo->fn_addr(fc);
        fc.isnull_result = false;
        return fc.result;
    }

    // Procedural language function: dispatch to the PL handler.
    if (finfo->fn_pl_handler != nullptr && finfo->fn_pl_handler->call_cb != nullptr) {
        fc.result = finfo->fn_pl_handler->call_cb(fc);
        if (fc.isnull_result && isnull != nullptr)
            *isnull = true;
        return fc.result;
    }

    // SQL-language function or no handler.
    if (isnull != nullptr)
        *isnull = true;
    return 0;
}

// --- DirectFunctionCallN ---

Datum DirectFunctionCall1(PgFunction func, Datum arg1) {
    FunctionCallInfo fc;
    fc.nargs = 1;
    fc.arg[0] = arg1;
    fc.result = func(fc);
    return fc.result;
}

Datum DirectFunctionCall2(PgFunction func, Datum arg1, Datum arg2) {
    FunctionCallInfo fc;
    fc.nargs = 2;
    fc.arg[0] = arg1;
    fc.arg[1] = arg2;
    fc.result = func(fc);
    return fc.result;
}

Datum DirectFunctionCall3(PgFunction func, Datum arg1, Datum arg2, Datum arg3) {
    FunctionCallInfo fc;
    fc.nargs = 3;
    fc.arg[0] = arg1;
    fc.arg[1] = arg2;
    fc.arg[2] = arg3;
    fc.result = func(fc);
    return fc.result;
}

// --- OidFunctionCallN ---

Datum OidFunctionCall1(Oid funcid, Datum arg1) {
    FmgrInfo finfo;
    if (!fmgr_info(funcid, &finfo))
        return 0;
    return FunctionCall(&finfo, {arg1});
}

Datum OidFunctionCall2(Oid funcid, Datum arg1, Datum arg2) {
    FmgrInfo finfo;
    if (!fmgr_info(funcid, &finfo))
        return 0;
    return FunctionCall(&finfo, {arg1, arg2});
}

Datum OidFunctionCall3(Oid funcid, Datum arg1, Datum arg2, Datum arg3) {
    FmgrInfo finfo;
    if (!fmgr_info(funcid, &finfo))
        return 0;
    return FunctionCall(&finfo, {arg1, arg2, arg3});
}

}  // namespace pgcpp::fmgr
