// pl_handler.h — Procedural Language (PL) handler interface.
//
// Converted from PostgreSQL 15's src/include/pl.h + commands/proclang.c.
//
// Each PL registers a PlHandler whose call_handler is invoked by fmgr
// whenever a function in that language is called. The handler is responsible
// for parsing the function body (prosrc) and executing it. This mirrors
// PostgreSQL's LanguageCallHandler convention.
//
// pgcpp extends the model with an inline_handler for DO blocks (anonymous
// code) so that the same PL can serve both CREATE FUNCTION ... LANGUAGE pl
// and DO $$ ... $$ LANGUAGE pl.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "types/datum.hpp"

// Forward declaration to avoid pulling the full fmgr header here.
namespace pgcpp::fmgr {
struct FunctionCallInfo;
}

namespace pgcpp::pl {

// PlHandler — the interface implemented by every procedural language.
//
// validate_cb: optional, called by CREATE FUNCTION to validate prosrc syntax.
//              May be null (no validation). Errors via ereport.
// call_cb:     invoked by FunctionCall when a function in this language is
//              called. Receives the FunctionCallInfo (with flinfo pointing to
//              the FmgrInfo, which in turn references the pg_proc row).
//              Must set fc.result / fc.isnull_result and return fc.result.
// inline_cb:   invoked by DO (anonymous code blocks). `code` is the source
//              text from the DO $$ ... $$ AS $$ ... $$ clause. May be null
//              (PL does not support DO).
struct PlHandler {
    pgcpp::catalog::Oid language_oid = 0;
    std::string language_name;

    // Validate the source text of a function. Called once at CREATE FUNCTION
    // time. May be null. Should ereport(kError, ...) on invalid source.
    void (*validate_cb)(const std::string& prosrc) = nullptr;

    // Call a stored function. Args are in fc.arg[0..nargs-1].
    pgcpp::types::Datum (*call_cb)(pgcpp::fmgr::FunctionCallInfo& fc) = nullptr;

    // Execute an anonymous code block (DO statement). Returns nothing;
    // results are side-effects only. May be null (PL does not support DO).
    void (*inline_cb)(const std::string& code) = nullptr;
};

// Register a PL handler. The handler must outlive the registry (typically
// it is a static). Registering a duplicate language_oid replaces the prior
// handler — useful for tests.
void RegisterPlHandler(const PlHandler* handler);

// Look up the PL handler for a given language OID. Returns nullptr if no
// handler is registered (e.g., for internal/c/sql).
const PlHandler* LookupPlHandler(pgcpp::catalog::Oid language_oid);

// Look up a PL handler by language name (e.g. "plpgsql"). Returns nullptr
// if the language is not a registered PL.
const PlHandler* LookupPlHandlerByName(const std::string& name);

// Clear all registered handlers (test-only; not used in production).
void ClearPlHandlers();

}  // namespace pgcpp::pl
