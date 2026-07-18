// pg_recvlogical.h — Logical replication receiver (pg_recvlogical).
//
// Converted from PostgreSQL 15's src/bin/pg_basebackup/pg_recvlogical.c.
//
// pg_recvlogical controls a logical replication slot on the server and/or
// receives a stream of logical replication messages (changes). It speaks
// the streaming-replication subset of the frontend/backend protocol.
//
// pgcpp's server does not yet implement a full logical decoding pipeline,
// so this port focuses on the SQL builders for slot management and a
// self-contained message parser. The end-to-end RunRecvlogical() will
// connect via libpq and issue the slot commands; message streaming falls
// back to polling SQL functions when the replication protocol is not
// available.
#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace pgcpp::tools {

// RecvlogicalAction — what to do.
enum class RecvlogicalAction {
    kCreate,  // create a slot (--create-slot)
    kDrop,    // drop a slot (--drop-slot)
    kStart,   // start streaming (--start)
    kStop,    // stop streaming (--stop)
    kInfo,    // show slot info (--slot-info)
};

// LogicalMsgType — kind of logical replication message (per PG protocol).
enum class LogicalMsgType : char {
    kBegin = 'B',     // BEGIN
    kCommit = 'C',    // COMMIT
    kOrigin = 'O',    // ORIGIN
    kRelation = 'R',  // RELATION
    kType = 'Y',      // TYPE
    kInsert = 'I',    // INSERT
    kUpdate = 'U',    // UPDATE
    kDelete = 'D',    // DELETE
    kTruncate = 'T',  // TRUNCATE
    kUnknown = '?',
};

// RecvlogicalOptions — inputs to a recvlogical action.
struct RecvlogicalOptions {
    RecvlogicalAction action = RecvlogicalAction::kStart;
    std::string host = "localhost";
    int port = 5432;
    std::string user;
    std::string dbname;
    std::string slot;                 // replication slot name
    std::string plugin = "pgoutput";  // plugin for --create-slot
    std::string output_file;          // file to write stream to (empty = stdout)
    std::int64_t start_lsn = 0;       // 0 = "no LSN specified"
    int status_interval = 10;         // seconds between status updates
    bool no_loop = false;             // exit at end of WAL rather than reconnect
    bool verbose = false;
    bool create_slot_if_needed = false;  // --if-not-exists
};

// RecvlogicalResult — outcome of an action.
enum class RecvlogicalResult {
    kOk,
    kConnectFailed,
    kSlotMissing,
    kSlotExists,
    kQueryFailed,
    kWriteFailed,
    kUnsupportedAction,
};

// RecvlogicalStats — counters accumulated during a start action.
struct RecvlogicalStats {
    int messages_received = 0;
    int transactions_received = 0;
    int inserts = 0;
    int updates = 0;
    int deletes = 0;
    int truncates = 0;
    std::int64_t bytes_received = 0;
};

// RunRecvlogical — perform the action described by `opts`.
// `out` (if non-null) is where the received stream is written; if null,
// the stream is discarded.
RecvlogicalResult RunRecvlogical(const RecvlogicalOptions& opts, RecvlogicalStats& stats,
                                 std::ostream* out);

// --- SQL builders (exposed for testing) ---

// BuildCreateSlotSql — "SELECT * FROM pg_create_logical_replication_slot(...)".
std::string BuildCreateSlotSql(const std::string& slot, const std::string& plugin);

// BuildDropSlotSql — "SELECT pg_drop_replication_slot('slot')".
std::string BuildDropSlotSql(const std::string& slot);

// BuildSlotInfoSql — "SELECT * FROM pg_replication_slots WHERE slot_name=...".
std::string BuildSlotInfoSql(const std::string& slot);

// BuildStartReplicationSql — "SELECT pg_logical_slot_get_changes('slot', ...)". This
// is the SQL-level fallback when the binary replication protocol is not used.
std::string BuildStartReplicationSql(const std::string& slot, std::int64_t start_lsn,
                                     int max_messages);

// BuildSlotPeekSql — like get_changes but doesn't advance the confirmed LSN.
std::string BuildSlotPeekSql(const std::string& slot, std::int64_t start_lsn, int max_messages);

// BuildAdvanceSlotSql — advance confirmed LSN without consuming messages.
std::string BuildAdvanceSlotSql(const std::string& slot, std::int64_t upto_lsn);

// --- Logical message parsing (exposed for testing) ---

// ParseMsgType — read the leading message-type char from a logical message.
// Returns kUnknown if the buffer is empty or the byte isn't recognized.
LogicalMsgType ParseMsgType(const std::vector<uint8_t>& buf);

// ParseBeginMessage — extract the XID from a BEGIN message (pgoutput format).
// Layout: 1-byte type ('B'), 8-byte LSN, 8-byte end LSN, 4-byte timestamp (sec),
// 4-byte timestamp (usec), 4-byte XID.
bool ParseBeginMessage(const std::vector<uint8_t>& buf, std::int64_t* lsn, std::int64_t* xid);

// ParseCommitMessage — extract LSN from a COMMIT message.
// Layout: 1-byte type ('C'), 1-byte flags, 8-byte LSN, 8-byte end LSN, 8-byte timestamp.
bool ParseCommitMessage(const std::vector<uint8_t>& buf, std::int64_t* lsn);

// ParseInsertMessage — extract the relation OID from an INSERT message.
// Layout: 1-byte type ('I'), 1-byte flags, 4-byte relation OID, ...
bool ParseInsertMessage(const std::vector<uint8_t>& buf, uint32_t* rel_oid);

// UpdateMessage — extract relation OID from an UPDATE message.
bool ParseUpdateMessage(const std::vector<uint8_t>& buf, uint32_t* rel_oid);

// ParseDeleteMessage — extract relation OID from a DELETE message.
bool ParseDeleteMessage(const std::vector<uint8_t>& buf, uint32_t* rel_oid);

// ParseTruncateMessage — extract relation count and OIDs from a TRUNCATE message.
bool ParseTruncateMessage(const std::vector<uint8_t>& buf, std::vector<uint32_t>* rel_oids);

// MsgTypeName — human-readable name of a LogicalMsgType.
const char* MsgTypeName(LogicalMsgType t);

}  // namespace pgcpp::tools
