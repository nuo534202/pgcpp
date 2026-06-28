// replutil.h — Shared utility types for the replication subsystem.
//
// Converted from PostgreSQL 15's src/backend/replication/replutil.c and
// the logical-replication message enum defined in libpq/pqformat.h and
// src/include/replication/logicalproto.h.
//
// This file holds the small shared types used by both the streaming
// (physical) and logical replication code paths: the LogicalRepMsgType
// enum that identifies each binary-protocol message on the wire, and
// LogicalRepTuple / LogicalRepRelation structures used by the logical
// apply worker to represent inbound rows.
//
// Network/IPC parts (libpq send/recv) are stubbed: the structures are
// in-process only. The enum values match PostgreSQL's wire format so
// future wire-level work can reuse them unchanged.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::replication {

// LogicalRepMsgType — message-type byte that prefixes every logical
// replication message on the wire. Values match PostgreSQL's
// LOGICAL_REP_MSG_* constants (a single ASCII char each).
enum class LogicalRepMsgType : uint8_t {
    kBegin = 'B',         // transaction begin
    kMessage = 'M',       // opaque plugin message
    kCommit = 'C',        // transaction commit
    kOrigin = 'O',        // origin record
    kRelation = 'R',      // relation metadata
    kType = 'Y',          // type metadata
    kInsert = 'I',        // insert row
    kUpdate = 'U',        // update row
    kDelete = 'D',        // delete row
    kTruncate = 'T',      // truncate relation(s)
    kStreamStart = 'S',   // streaming-transactions: stream start
    kStreamStop = 'E',    // streaming-transactions: stream stop
    kStreamCommit = 'c',  // streaming-transactions: stream commit
    kStreamAbort = 'a',   // streaming-transactions: stream abort
};

// Convert a LogicalRepMsgType to its single-character string form ("B", "I", ...).
const char* LogicalRepMsgTypeName(LogicalRepMsgType t);

// LogicalRepTuple — a single row represented as an array of column values.
// Each entry is the text representation of the column (or empty when null,
// as indicated by the matching entry in is_null). Matches PostgreSQL's
// logical_replication_tuple structure conceptually.
struct LogicalRepTuple {
    std::vector<std::string> values;
    std::vector<bool> is_null;

    void Clear() {
        values.clear();
        is_null.clear();
    }

    void Reserve(std::size_t n) {
        values.reserve(n);
        is_null.reserve(n);
    }

    void Append(std::string v, bool null = false) {
        values.push_back(std::move(v));
        is_null.push_back(null);
    }
};

// LogicalRepRelation — description of a relation known to the apply worker.
struct LogicalRepRelation {
    // Remote Oid of the relation (as sent by the publisher).
    uint32_t remote_oid = 0;
    // Schema-qualified relation name (e.g. "public.t1").
    std::string nspname;
    std::string relname;
    // Number of columns / replica identity columns.
    int natts = 0;
    // Per-column "is this column part of the replica identity?" flag.
    std::vector<bool> attis_key;
    // Per-column type names (text form for diagnostics).
    std::vector<std::string> atttypnames;
};

// WalLevel — minimal `wal_level` enum (PG: minimal/replica/logical).
enum class WalLevel : int {
    kMinimal = 0,
    kReplica = 1,
    kLogical = 2,
};

// GetWalLevel / SetWalLevel — runtime `wal_level` accessors (default: replica).
WalLevel GetWalLevel();
void SetWalLevel(WalLevel level);

// LogicalEncodingName — the canonical output plugin name to use when none
// is specified explicitly (matches PG's "pgoutput" default).
const char* DefaultOutputPluginName();

}  // namespace pgcpp::replication
