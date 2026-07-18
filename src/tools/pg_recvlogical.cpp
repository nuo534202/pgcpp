// pg_recvlogical.cpp — Logical replication receiver (pg_recvlogical).
//
// Implements the slot management SQL builders and the logical-message parser.
// RunRecvlogical connects via libpq and issues slot commands; full streaming
// reception is delegated to pg_logical_slot_get_changes (the SQL fallback).
#include "tools/pg_recvlogical.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "libpq/libpq.hpp"

namespace pgcpp::tools {

namespace {

// QuoteLiteral — wrap in single quotes, doubling any embedded '.
std::string QuoteLiteral(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'')
            out.push_back('\'');
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

// RunSql — execute `sql` on `conn`, return the rows of the result.
// On error, returns false and sets *err_msg.
bool RunSql(pgcpp::libpq::PgConn& conn, const std::string& sql,
            std::vector<std::vector<std::string>>& rows, std::string* err_msg) {
    pgcpp::libpq::PgResult r = conn.Exec(sql);
    if (r.Status() == pgcpp::libpq::ExecStatusType::kFatalError) {
        if (err_msg)
            *err_msg = r.ErrorMessage();
        return false;
    }
    int n = r.NTuples();
    int nfields = r.NFields();
    rows.clear();
    rows.reserve(n);
    for (int i = 0; i < n; ++i) {
        std::vector<std::string> row;
        row.reserve(nfields);
        for (int j = 0; j < nfields; ++j) {
            const char* v = r.GetValue(i, j);
            row.emplace_back(v ? v : "");
        }
        rows.push_back(std::move(row));
    }
    return true;
}

// ReadBE32 — read a 4-byte big-endian uint32 starting at offset `off`.
bool ReadBE32(const std::vector<uint8_t>& buf, std::size_t off, uint32_t* out) {
    if (off + 4 > buf.size())
        return false;
    *out = (static_cast<uint32_t>(buf[off]) << 24) | (static_cast<uint32_t>(buf[off + 1]) << 16) |
           (static_cast<uint32_t>(buf[off + 2]) << 8) | (static_cast<uint32_t>(buf[off + 3]));
    return true;
}

// ReadBE64 — read an 8-byte big-endian uint64 starting at offset `off`.
bool ReadBE64(const std::vector<uint8_t>& buf, std::size_t off, std::int64_t* out) {
    if (off + 8 > buf.size())
        return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | static_cast<uint64_t>(buf[off + i]);
    *out = static_cast<std::int64_t>(v);
    return true;
}

}  // namespace

std::string BuildCreateSlotSql(const std::string& slot, const std::string& plugin) {
    std::ostringstream oss;
    oss << "SELECT * FROM pg_create_logical_replication_slot(" << QuoteLiteral(slot) << ", "
        << QuoteLiteral(plugin) << ");";
    return oss.str();
}

std::string BuildDropSlotSql(const std::string& slot) {
    return "SELECT pg_drop_replication_slot(" + QuoteLiteral(slot) + ");";
}

std::string BuildSlotInfoSql(const std::string& slot) {
    return "SELECT slot_name, plugin, slot_type, active, restart_lsn, confirmed_flush_lsn "
           "FROM pg_replication_slots WHERE slot_name = " +
           QuoteLiteral(slot) + ";";
}

std::string BuildStartReplicationSql(const std::string& slot, std::int64_t start_lsn,
                                     int max_messages) {
    std::ostringstream oss;
    oss << "SELECT lsn, xid, data FROM pg_logical_slot_get_changes(" << QuoteLiteral(slot) << ", ";
    if (start_lsn == 0)
        oss << "NULL";
    else
        oss << "'" << start_lsn << "'";
    oss << ", " << max_messages << ", NULL);";
    return oss.str();
}

std::string BuildSlotPeekSql(const std::string& slot, std::int64_t start_lsn, int max_messages) {
    std::ostringstream oss;
    oss << "SELECT lsn, xid, data FROM pg_logical_slot_peek_changes(" << QuoteLiteral(slot) << ", ";
    if (start_lsn == 0)
        oss << "NULL";
    else
        oss << "'" << start_lsn << "'";
    oss << ", " << max_messages << ", NULL);";
    return oss.str();
}

std::string BuildAdvanceSlotSql(const std::string& slot, std::int64_t upto_lsn) {
    std::ostringstream oss;
    oss << "SELECT * FROM pg_replication_slot_advance(" << QuoteLiteral(slot) << ", '" << upto_lsn
        << "');";
    return oss.str();
}

LogicalMsgType ParseMsgType(const std::vector<uint8_t>& buf) {
    if (buf.empty())
        return LogicalMsgType::kUnknown;
    char c = static_cast<char>(buf[0]);
    switch (c) {
        case 'B':
            return LogicalMsgType::kBegin;
        case 'C':
            return LogicalMsgType::kCommit;
        case 'O':
            return LogicalMsgType::kOrigin;
        case 'R':
            return LogicalMsgType::kRelation;
        case 'Y':
            return LogicalMsgType::kType;
        case 'I':
            return LogicalMsgType::kInsert;
        case 'U':
            return LogicalMsgType::kUpdate;
        case 'D':
            return LogicalMsgType::kDelete;
        case 'T':
            return LogicalMsgType::kTruncate;
        default:
            return LogicalMsgType::kUnknown;
    }
}

bool ParseBeginMessage(const std::vector<uint8_t>& buf, std::int64_t* lsn, std::int64_t* xid) {
    // Type(1) + LSN(8) + EndLSN(8) + TimeSec(4) + TimeUsec(4) + XID(4).
    if (buf.size() < 1 + 8 + 8 + 4 + 4 + 4)
        return false;
    if (buf[0] != 'B')
        return false;
    if (!ReadBE64(buf, 1, lsn))
        return false;
    uint32_t xid_be = 0;
    if (!ReadBE32(buf, 1 + 8 + 8 + 4 + 4, &xid_be))
        return false;
    *xid = static_cast<std::int64_t>(xid_be);
    return true;
}

bool ParseCommitMessage(const std::vector<uint8_t>& buf, std::int64_t* lsn) {
    // Type(1) + Flags(1) + LSN(8) + EndLSN(8) + Time(8).
    if (buf.size() < 1 + 1 + 8 + 8 + 8)
        return false;
    if (buf[0] != 'C')
        return false;
    return ReadBE64(buf, 2, lsn);
}

bool ParseInsertMessage(const std::vector<uint8_t>& buf, uint32_t* rel_oid) {
    // Type(1) + Flags(1) + RelOID(4) + ...
    if (buf.size() < 1 + 1 + 4)
        return false;
    if (buf[0] != 'I')
        return false;
    return ReadBE32(buf, 2, rel_oid);
}

bool ParseUpdateMessage(const std::vector<uint8_t>& buf, uint32_t* rel_oid) {
    if (buf.size() < 1 + 1 + 4)
        return false;
    if (buf[0] != 'U')
        return false;
    return ReadBE32(buf, 2, rel_oid);
}

bool ParseDeleteMessage(const std::vector<uint8_t>& buf, uint32_t* rel_oid) {
    if (buf.size() < 1 + 1 + 4)
        return false;
    if (buf[0] != 'D')
        return false;
    return ReadBE32(buf, 2, rel_oid);
}

bool ParseTruncateMessage(const std::vector<uint8_t>& buf, std::vector<uint32_t>* rel_oids) {
    // Type(1) + Flags(1) + RelCount(4) + RelOID[RelCount].
    if (buf.size() < 1 + 1 + 4)
        return false;
    if (buf[0] != 'T')
        return false;
    uint32_t count = 0;
    if (!ReadBE32(buf, 2, &count))
        return false;
    std::size_t need = 1 + 1 + 4 + count * 4;
    if (buf.size() < need)
        return false;
    rel_oids->clear();
    rel_oids->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t oid = 0;
        if (!ReadBE32(buf, 6 + i * 4, &oid))
            return false;
        rel_oids->push_back(oid);
    }
    return true;
}

const char* MsgTypeName(LogicalMsgType t) {
    switch (t) {
        case LogicalMsgType::kBegin:
            return "BEGIN";
        case LogicalMsgType::kCommit:
            return "COMMIT";
        case LogicalMsgType::kOrigin:
            return "ORIGIN";
        case LogicalMsgType::kRelation:
            return "RELATION";
        case LogicalMsgType::kType:
            return "TYPE";
        case LogicalMsgType::kInsert:
            return "INSERT";
        case LogicalMsgType::kUpdate:
            return "UPDATE";
        case LogicalMsgType::kDelete:
            return "DELETE";
        case LogicalMsgType::kTruncate:
            return "TRUNCATE";
        case LogicalMsgType::kUnknown:
            return "UNKNOWN";
    }
    return "UNKNOWN";
}

RecvlogicalResult RunRecvlogical(const RecvlogicalOptions& opts, RecvlogicalStats& stats,
                                 std::ostream* out) {
    if (opts.slot.empty())
        return RecvlogicalResult::kSlotMissing;

    pgcpp::libpq::ConnectOptions co;
    co.host = opts.host;
    co.port = opts.port;
    co.user = opts.user;
    co.dbname = opts.dbname;
    pgcpp::libpq::PgConn conn;
    if (conn.Connect(co) != pgcpp::libpq::ConnStatusType::kOk)
        return RecvlogicalResult::kConnectFailed;

    std::string err;
    std::vector<std::vector<std::string>> rows;

    switch (opts.action) {
        case RecvlogicalAction::kCreate: {
            if (!RunSql(conn, BuildCreateSlotSql(opts.slot, opts.plugin), rows, &err)) {
                conn.Finish();
                return RecvlogicalResult::kSlotExists;  // likely already exists
            }
            conn.Finish();
            return RecvlogicalResult::kOk;
        }
        case RecvlogicalAction::kDrop: {
            if (!RunSql(conn, BuildDropSlotSql(opts.slot), rows, &err)) {
                conn.Finish();
                return RecvlogicalResult::kSlotMissing;  // likely doesn't exist
            }
            conn.Finish();
            return RecvlogicalResult::kOk;
        }
        case RecvlogicalAction::kInfo: {
            if (!RunSql(conn, BuildSlotInfoSql(opts.slot), rows, &err)) {
                conn.Finish();
                return RecvlogicalResult::kQueryFailed;
            }
            if (out) {
                for (const auto& row : rows) {
                    for (std::size_t i = 0; i < row.size(); ++i) {
                        if (i > 0)
                            *out << " | ";
                        *out << row[i];
                    }
                    *out << "\n";
                }
            }
            conn.Finish();
            if (rows.empty())
                return RecvlogicalResult::kSlotMissing;
            return RecvlogicalResult::kOk;
        }
        case RecvlogicalAction::kStop:
            // Stop is a no-op on the SQL fallback — no streaming was started.
            conn.Finish();
            return RecvlogicalResult::kOk;
        case RecvlogicalAction::kStart: {
            // Use pg_logical_slot_get_changes to fetch the next batch.
            const int kMaxMessages = 10000;
            if (!RunSql(conn, BuildStartReplicationSql(opts.slot, opts.start_lsn, kMaxMessages),
                        rows, &err)) {
                conn.Finish();
                return RecvlogicalResult::kQueryFailed;
            }
            for (const auto& row : rows) {
                if (row.size() < 3)
                    continue;
                ++stats.messages_received;
                stats.bytes_received += static_cast<std::int64_t>(row[2].size());
                // The "data" column contains the message bytes (in hex for
                // pgoutput). For now, just count by message-type char.
                if (!row[2].empty()) {
                    char t = row[2][0];
                    switch (t) {
                        case 'B':
                            ++stats.transactions_received;
                            break;
                        case 'I':
                            ++stats.inserts;
                            break;
                        case 'U':
                            ++stats.updates;
                            break;
                        case 'D':
                            ++stats.deletes;
                            break;
                        case 'T':
                            ++stats.truncates;
                            break;
                        default:
                            break;
                    }
                }
                if (out) {
                    *out << "lsn=" << row[0] << " xid=" << row[1] << " data=" << row[2] << "\n";
                }
            }
            conn.Finish();
            return RecvlogicalResult::kOk;
        }
    }
    return RecvlogicalResult::kUnsupportedAction;
}

}  // namespace pgcpp::tools
