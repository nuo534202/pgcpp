// logical.h — Logical decoding context.
//
// Converted from PostgreSQL 15's src/backend/replication/logical/ logical.c
// (LogicalDecodingContext and the CreateInitDecodingContext /
// CreateDecodingContext entry points). The actual decoding loop is
// stubbed: MyToyDB does not yet replay WAL through an output plugin, so
// LogicalShippingMain returns immediately after constructing a context.
//
// A LogicalDecodingContext bundles:
//   - the output plugin name ("pgoutput" by default),
//   - a small set of options (debug, no_txn_cb, streaming),
//   - a target slot name (must already exist),
//   - the next LSN the plugin expects to decode from.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pgcpp/replication/replutil.hpp"
#include "pgcpp/transaction/xlog.hpp"

namespace mytoydb::replication {

// LogicalDecodingOptions — flags passed to CreateInitDecodingContext /
// CreateDecodingContext. Mirrors PG's logical_streaming_xact_cb / options.
struct LogicalDecodingOptions {
    bool emit_no_xlog = false;  // don't write logical decoding changes
    bool skip_begin = false;    // skip emitting BEGIN
    bool streaming = false;     // allow streaming of in-progress xacts
    bool two_phase = false;     // allow two-phase commit decoding
    bool include_lsn = false;   // include LSN in messages
};

// LogicalDecodingContext — bundle of plugin state used during decoding.
struct LogicalDecodingContext {
    std::string plugin_name;  // e.g. "pgoutput"
    std::string slot_name;    // must already exist
    LogicalDecodingOptions options;
    transaction::XLogRecPtr start_lsn = 0;  // where decoding started
    transaction::XLogRecPtr end_lsn = 0;    // where decoding stopped
    bool prepared = false;                  // context fully initialized
    // Writer callback target (in PG: a WriteData fp). Here just a string
    // accumulator so callers/tests can inspect what would be sent.
    std::string output_buffer;
    std::vector<std::string> messages;  // one entry per "decoded" message
};

// CreateInitDecodingContext — create a brand-new logical slot and prepare
// a decoding context for it. Returns a context describing the new slot.
// On error (slot already exists, plugin missing), calls ereport(ERROR).
LogicalDecodingContext CreateInitDecodingContext(const std::string& plugin_name,
                                                 const std::string& slot_name,
                                                 const std::string& database,
                                                 LogicalDecodingOptions options);

// CreateDecodingContext — open an existing logical slot for decoding.
// Returns a context bound to the slot. On error (missing slot, slot is
// physical, slot already in use), calls ereport(ERROR).
LogicalDecodingContext CreateDecodingContext(const std::string& slot_name,
                                             LogicalDecodingOptions options);

// LogicalShippingMain — main loop of the logical shipping process
// (PG: LogicalStreamingMain / walsender+logical mode). Stub: returns
// immediately with the prepared context (no actual decoding performed).
// `max_messages` is honored only as a cap on synthesized messages.
int LogicalShippingMain(LogicalDecodingContext& ctx, int max_messages);

// LogicalDecodingReset — clear all in-process decoding state (for tests).
void LogicalDecodingReset();

// GetLogicalDecodingContext — return a pointer to the most recently
// created context (for tests/diagnostics). May be nullptr.
LogicalDecodingContext* GetLogicalDecodingContext();

// DecodingEmitMessage — push a synthesized message into the context's
// output buffer. Used by the (stubbed) decoding loop.
void DecodingEmitMessage(LogicalDecodingContext& ctx, LogicalRepMsgType type,
                         const std::string& payload);

}  // namespace mytoydb::replication
