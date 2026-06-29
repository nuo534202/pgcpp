// dest.h — DestReceiver: polymorphic destination for query result tuples.
//
// Converted from PostgreSQL 15's src/backend/tcop/dest.c.
//
// A DestReceiver is the abstraction used by the executor to send result
// tuples to a destination (the frontend client, an in-memory tuplestore,
// a new relation, etc.). The executor calls:
//   rStartup    — once before the first tuple (passes the tuple descriptor)
//   receiveSlot — once per result tuple
//   rShutdown   — once after the last tuple
//   rDestroy    — release the receiver itself
//
// PostgreSQL implements DestReceiver as a struct of function pointers; the
// C++ port uses virtual methods. The CommandDest enum and the factory
// CreateDestReceiver mirror the C API for compatibility.
#pragma once

#include <vector>

#include "catalog/catalog.hpp"    // Oid
#include "protocol/pqformat.hpp"  // OutputSink, Message
#include "types/datum.hpp"        // Datum

namespace pgcpp::access {
struct TupleDescData;
using TupleDesc = TupleDescData*;
}  // namespace pgcpp::access

namespace pgcpp::executor {
struct QueryDesc;
struct TupleTableSlot;
}  // namespace pgcpp::executor

namespace pgcpp::protocol {

// CommandDest — identifies the destination for query results.
enum class CommandDest {
    kNone,           // discard results
    kDebug,          // pretty-print to stdout
    kRemote,         // send to client via simple-query protocol (with RowDescription)
    kRemoteExecute,  // send to client via extended-query protocol (no RowDescription)
    kTuplestore,     // collect into in-memory tuplestore
    kIntoRel,        // INSERT into a new relation (CREATE TABLE AS SELECT)
    kSQLFunction,    // collect for SQL-language function
};

// DestReceiver — polymorphic destination for query result tuples.
// Mirrors PG's DestReceiver struct (function-pointer table) using C++ virtual
// methods. Concrete receivers are created via the Create*Receiver helpers.
class DestReceiver {
public:
    virtual ~DestReceiver() = default;

    // Called once before the first tuple (setup).
    // `operation` is the query's command type (CmdType as int).
    virtual void rStartup(pgcpp::executor::QueryDesc* /*query_desc*/, int /*operation*/,
                          pgcpp::access::TupleDesc /*tupdesc*/) {}
    // Called for each result tuple. Returns true to continue, false to stop.
    virtual bool receiveSlot(pgcpp::executor::TupleTableSlot* /*slot*/,
                             pgcpp::executor::QueryDesc* /*query_desc*/) {
        return true;
    }
    // Called once after the last tuple (teardown).
    virtual void rShutdown(pgcpp::executor::QueryDesc* /*query_desc*/) {}
    // Destroy the receiver (release resources).
    virtual void rDestroy() {}

    // The CommandDest this receiver was created for.
    CommandDest mydest = CommandDest::kNone;
};

// Factory: create a DestReceiver for the given destination.
// `sink` is used by kRemote / kRemoteExecute receivers (ignored otherwise).
// For kIntoRel, use CreateIntoRelReceiver directly (the relid is required).
// The caller is responsible for calling rDestroy() (or deleting the pointer).
DestReceiver* CreateDestReceiver(CommandDest dest, OutputSink* sink);

// --- Concrete receiver constructors (for direct use) ---
// Each returns a heap-allocated receiver (owned by the caller).

// NoneReceiver — discards all rows (DestNone).
DestReceiver* CreateNoneReceiver();

// DebugReceiver — prints tuples to stdout (DestDebug, simplified).
DestReceiver* CreateDebugReceiver();

// RemoteReceiver — sends RowDescription + DataRow via OutputSink (DestRemote).
// `send_row_description` controls whether RowDescription is sent in rStartup
// (true for simple query, false for extended query).
DestReceiver* CreateRemoteReceiver(OutputSink* sink, bool send_row_description);

// TuplestoreReceiver — collects TupleTableSlots into an in-memory vector.
DestReceiver* CreateTuplestoreReceiver();

// IntoRelReceiver — inserts each tuple into a new relation.
// `relid` is the OID of the target relation (already created).
// Used by CREATE TABLE AS SELECT.
DestReceiver* CreateIntoRelReceiver(pgcpp::catalog::Oid relid);

// --- Helpers for accessing tuplestore receiver results ---

// Returns the collected slots from a TuplestoreReceiver (empty otherwise).
// The slots are owned by the receiver and freed on rDestroy.
std::vector<pgcpp::executor::TupleTableSlot*> GetTuplestoreSlots(DestReceiver* receiver);

// --- Datum encoding helper ---
// Encode a Datum to its text representation based on the type OID.
// Shared between RemoteReceiver and other callers that need text output.
std::string EncodeDatumAsText(pgcpp::types::Datum value, pgcpp::catalog::Oid type_oid);

}  // namespace pgcpp::protocol
