// logicalproto.cpp — Logical message WAL records.
//
// Converted from PostgreSQL 15's src/backend/replication/logical/message.c.
//
// LogLogicalMessage assembles an xl_logical_message and writes it to the WAL
// via XLogInsert with rmid = kRmgrLogicalMsgId. The output plugin later
// reads these records during logical decoding.
#include "replication/logicalproto.hpp"

#include <cstring>
#include <vector>

#include "transaction/transam.hpp"
#include "transaction/xlog.hpp"
#include "transaction/xloginsert.hpp"

namespace pgcpp::replication {

using pgcpp::transaction::kRmgrLogicalMsgId;
using pgcpp::transaction::XLogBeginInsert;
using pgcpp::transaction::XLogInsert;
using pgcpp::transaction::XLogRecPtr;
using pgcpp::transaction::XLogRegisterData;

std::vector<uint8_t> SerializeLogicalMessage(const xl_logical_message& msg) {
    // Layout: db_id(4) + transactional(1) + prefix_size(4) + message_size(4)
    //         + prefix bytes + message bytes
    std::vector<uint8_t> buf;
    buf.reserve(4 + 1 + 4 + 4 + msg.prefix.size() + msg.message.size());

    uint32_t db_id = msg.db_id;
    uint8_t txn_flag = msg.transactional ? 1 : 0;
    uint32_t prefix_size = static_cast<uint32_t>(msg.prefix.size());
    uint32_t message_size = static_cast<uint32_t>(msg.message.size());

    const auto* p = reinterpret_cast<const uint8_t*>(&db_id);
    buf.insert(buf.end(), p, p + sizeof(db_id));
    buf.push_back(txn_flag);
    p = reinterpret_cast<const uint8_t*>(&prefix_size);
    buf.insert(buf.end(), p, p + sizeof(prefix_size));
    p = reinterpret_cast<const uint8_t*>(&message_size);
    buf.insert(buf.end(), p, p + sizeof(message_size));
    buf.insert(buf.end(), msg.prefix.begin(), msg.prefix.end());
    buf.insert(buf.end(), msg.message.begin(), msg.message.end());
    return buf;
}

bool ParseLogicalMessage(const uint8_t* data, std::size_t len, xl_logical_message& out) {
    if (data == nullptr || len < 4 + 1 + 4 + 4) {
        return false;
    }
    std::size_t off = 0;
    std::memcpy(&out.db_id, data + off, sizeof(uint32_t));
    off += sizeof(uint32_t);
    out.transactional = (data[off] != 0);
    off += 1;
    uint32_t prefix_size = 0;
    std::memcpy(&prefix_size, data + off, sizeof(uint32_t));
    off += sizeof(uint32_t);
    uint32_t message_size = 0;
    std::memcpy(&message_size, data + off, sizeof(uint32_t));
    off += sizeof(uint32_t);

    // Bounds-check: prefix + message must fit in remaining bytes.
    if (off + prefix_size + message_size > len) {
        return false;
    }
    out.prefix.assign(reinterpret_cast<const char*>(data + off), prefix_size);
    off += prefix_size;
    out.message.assign(reinterpret_cast<const char*>(data + off), message_size);
    return true;
}

XLogRecPtr LogLogicalMessage(bool transactional, const std::string& prefix,
                             const std::string& message) {
    xl_logical_message msg;
    msg.db_id = 0;  // pgcpp doesn't track database OIDs yet
    msg.transactional = transactional;
    msg.prefix = prefix;
    msg.message = message;

    std::vector<uint8_t> payload = SerializeLogicalMessage(msg);

    XLogBeginInsert();
    XLogRegisterData(payload.data(), payload.size());
    return XLogInsert(kRmgrLogicalMsgId, kXlogLogicalMessage);
}

}  // namespace pgcpp::replication
