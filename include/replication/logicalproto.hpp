// logicalproto.h — Logical message WAL records and logical decoding protocol.
//
// Converted from PostgreSQL 15's src/include/replication/logicalproto.h and
// src/backend/replication/logical/message.c.
//
// A logical message is an arbitrary payload written to the WAL with
// rmid = kRmgrLogicalMsgId. It is the entry point for logical decoding:
// the output plugin sees these records and can forward them to subscribers.
//
// pgcpp implements two flavors (matching PG):
//   - transactional: the message is part of the surrounding transaction
//     and is only decoded on COMMIT. If the transaction aborts, the message
//     is discarded.
//   - non-transactional: the message is decoded immediately, regardless of
//     transaction boundaries.
//
// WAL record layout for rmid = kRmgrLogicalMsgId:
//   [XLogRecord header (24 bytes)]
//   [main_data: xl_logical_message header + prefix + payload]
//
// xl_logical_message (main_data) layout:
//   [db_id (4)] [transactional (1)] [prefix_size (4)] [message_size (4)]
//   [prefix bytes] [message bytes]
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "transaction/xlog.hpp"
#include "transaction/xloginsert.hpp"

namespace pgcpp::replication {

// XLOG_LOGICAL_MESSAGE — info byte for logical message WAL records.
// Matches PostgreSQL's XLOG_LOGICAL_MESSAGE (0).
constexpr uint8_t kXlogLogicalMessage = 0;

// xl_logical_message — WAL record payload for a logical message.
// Serialized as: db_id(4) + transactional(1) + prefix_size(4) + message_size(4)
// followed by prefix bytes and message bytes.
struct xl_logical_message {
    uint32_t db_id = 0;          // database OID (for filtering)
    bool transactional = false;  // true: emit within surrounding txn
    std::string prefix;          // logical message prefix (e.g. plugin name)
    std::string message;         // message payload
};

// LogLogicalMessage — write a logical message to the WAL.
// Returns the LSN of the inserted record.
transaction::XLogRecPtr LogLogicalMessage(bool transactional, const std::string& prefix,
                                          const std::string& message);

// ParseLogicalMessage — deserialize an xl_logical_message from a WAL record's
// main_data. Returns true on success, false on malformed input.
bool ParseLogicalMessage(const uint8_t* data, std::size_t len, xl_logical_message& out);

// SerializeLogicalMessage — serialize an xl_logical_message into a byte vector
// (used by LogLogicalMessage and tests).
std::vector<uint8_t> SerializeLogicalMessage(const xl_logical_message& msg);

}  // namespace pgcpp::replication
