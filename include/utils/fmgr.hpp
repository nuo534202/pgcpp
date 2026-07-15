// fmgr.h — Function manager (fmgr) infrastructure.
//
// Converted from PostgreSQL 15's src/include/fmgr.h.
//
// The function manager provides a uniform calling convention for all
// PostgreSQL functions, whether they are built-in (internal language),
// SQL-language, or C-language. Instead of dispatching by name (string
// comparison), the executor uses FmgrInfo to call any function by OID
// through a single FunctionCall() entry point.
//
// Key types:
//   FmgrInfo       — cached function metadata + dispatch pointer
//   FunctionCallInfo — argument vector + result slot for a single call
//
// Key API:
//   fmgr_info(oid, &finfo)     — look up a function by OID, fill FmgrInfo
//   FunctionCall(&finfo, args) — call the function, return Datum
//   DirectFunctionCallN(...)  — call by C function pointer directly
//   OidFunctionCallN(oid,...) — look up by OID + call in one step
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "types/datum.hpp"

// Forward declaration: PL handler interface (defined in pl/pl_handler.hpp).
// FmgrInfo stores a pointer to the handler for procedural-language functions.
namespace pgcpp::pl {
struct PlHandler;
}

namespace pgcpp::fmgr {

// Language OIDs (PostgreSQL standard values from pg_language).
// These are used to dispatch function calls by language.
constexpr pgcpp::catalog::Oid kInternalLanguageOid = 12;
constexpr pgcpp::catalog::Oid kCLanguageOid = 13;
constexpr pgcpp::catalog::Oid kSqlLanguageOid = 14;
// plpgsql language OID. PostgreSQL assigns this dynamically at initdb time;
// pgcpp pins it to 100 (matching BootstrapCatalog's pg_language row) so
// fmgr_info can dispatch PL/pgSQL calls without consulting the catalog.
constexpr pgcpp::catalog::Oid kPlPgsqlLanguageOid = 100;

// PgFunction — pointer to a built-in function that takes a FunctionCallInfo
// and returns a Datum. This is the C-function calling convention shared by
// internal and C-language functions.
struct FunctionCallInfo;
using PgFunction = pgcpp::types::Datum (*)(FunctionCallInfo&);

// FunctionCallInfo — the argument vector passed to a function.
// Following PostgreSQL's FunctionCallInfoData layout (simplified).
struct FunctionCallInfo {
    // The FmgrInfo for the function being called (set by FunctionCall).
    const struct FmgrInfo* flinfo = nullptr;

    // Number of arguments actually passed.
    int nargs = 0;

    // Argument values and null flags. arg[i] is valid only if !isnull[i].
    pgcpp::types::Datum arg[10] = {};
    bool isnull[10] = {};

    // Result.
    pgcpp::types::Datum result = 0;
    bool isnull_result = false;
};

// FmgrInfo — cached function metadata + dispatch pointer.
// The executor fills this once (via fmgr_info) and reuses it for every call.
struct FmgrInfo {
    pgcpp::catalog::Oid fn_oid = 0;       // pg_proc OID
    PgFunction fn_addr = nullptr;         // C function pointer (internal/C only)
    pgcpp::catalog::Oid fn_language = 0;  // pg_language OID
    bool fn_strict = true;                // if true, return NULL when any arg is NULL
    std::string fn_name;                  // proname (for SQL dispatch / error messages)

    // PL handler pointer (procedural languages only). When non-null and
    // fn_addr is null, FunctionCall dispatches to fn_pl_handler->call_cb.
    // Set by fmgr_info when the function's language is a registered PL.
    const pgcpp::pl::PlHandler* fn_pl_handler = nullptr;

    // True if a call can be dispatched — either via fn_addr (internal/C
    // language with a registered builtin) or via fn_pl_handler (a
    // registered procedural language like plpgsql). False for SQL-language
    // functions, which have no C handler in pgcpp's MVP.
    bool has_handler() const { return fn_addr != nullptr || fn_pl_handler != nullptr; }
};

// fmgr_info — look up a function by OID and fill FmgrInfo.
// Returns true on success, false if the function OID is not found.
bool fmgr_info(pgcpp::catalog::Oid funcid, FmgrInfo* finfo);

// FunctionCall — call a function via its FmgrInfo.
// Handles strict-function NULL short-circuiting.
pgcpp::types::Datum FunctionCall(const FmgrInfo* finfo,
                                 const std::vector<pgcpp::types::Datum>& args,
                                 bool* isnull = nullptr);

// FunctionCallWithNulls — call a function with explicit NULL flags.
pgcpp::types::Datum FunctionCallWithNulls(const FmgrInfo* finfo,
                                          const std::vector<pgcpp::types::Datum>& args,
                                          const std::vector<bool>& isnulls, bool* isnull = nullptr);

// DirectFunctionCall1/2/3 — call a C function pointer directly (no OID
// lookup, no strict check). Used internally by operator implementations.
pgcpp::types::Datum DirectFunctionCall1(PgFunction func, pgcpp::types::Datum arg1);
pgcpp::types::Datum DirectFunctionCall2(PgFunction func, pgcpp::types::Datum arg1,
                                        pgcpp::types::Datum arg2);
pgcpp::types::Datum DirectFunctionCall3(PgFunction func, pgcpp::types::Datum arg1,
                                        pgcpp::types::Datum arg2, pgcpp::types::Datum arg3);

// OidFunctionCall1/2/3 — look up a function by OID and call it.
// Convenience wrappers combining fmgr_info + FunctionCall.
pgcpp::types::Datum OidFunctionCall1(pgcpp::catalog::Oid funcid, pgcpp::types::Datum arg1);
pgcpp::types::Datum OidFunctionCall2(pgcpp::catalog::Oid funcid, pgcpp::types::Datum arg1,
                                     pgcpp::types::Datum arg2);
pgcpp::types::Datum OidFunctionCall3(pgcpp::catalog::Oid funcid, pgcpp::types::Datum arg1,
                                     pgcpp::types::Datum arg2, pgcpp::types::Datum arg3);

// LookupBuiltinFunction — find a C function pointer by function name.
// Returns nullptr if no builtin is registered for the name.
// This is used by fmgr_info to resolve internal-language functions.
PgFunction LookupBuiltinFunction(const std::string& proname);

}  // namespace pgcpp::fmgr
