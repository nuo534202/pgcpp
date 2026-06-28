// pqformat.cpp — PostgreSQL frontend/backend protocol message encoding and decoding.
//
// Converted from PostgreSQL 15's src/backend/libpq/pqformat.c.
//
// Implements the MessageWriter/MessageReader classes and the convenience
// message builder functions declared in pqformat.h.
#include "pgcpp/protocol/pqformat.hpp"

#include <cstring>

#include "pgcpp/common/error/elog.hpp"

namespace mytoydb::protocol {

// --- Message ---

std::string Message::BuildWireFormat() const {
    std::string out;
    out.reserve(1 + 4 + payload.size());
    out.push_back(static_cast<char>(type));
    int32_t len = static_cast<int32_t>(4 + payload.size());
    // Network byte order (big-endian).
    out.push_back(static_cast<char>((len >> 24) & 0xFF));
    out.push_back(static_cast<char>((len >> 16) & 0xFF));
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
    out.append(payload);
    return out;
}

// --- MessageWriter ---

void MessageWriter::WriteByte(char b) {
    buf_.push_back(b);
}

void MessageWriter::WriteBytes(const char* data, std::size_t len) {
    buf_.append(data, len);
}

void MessageWriter::WriteInt16(int16_t v) {
    uint16_t u = static_cast<uint16_t>(v);
    buf_.push_back(static_cast<char>((u >> 8) & 0xFF));
    buf_.push_back(static_cast<char>(u & 0xFF));
}

void MessageWriter::WriteInt32(int32_t v) {
    uint32_t u = static_cast<uint32_t>(v);
    buf_.push_back(static_cast<char>((u >> 24) & 0xFF));
    buf_.push_back(static_cast<char>((u >> 16) & 0xFF));
    buf_.push_back(static_cast<char>((u >> 8) & 0xFF));
    buf_.push_back(static_cast<char>(u & 0xFF));
}

void MessageWriter::WriteString(const std::string& s) {
    buf_.append(s);
    buf_.push_back('\0');
}

void MessageWriter::WriteStringN(const std::string& s, std::size_t len) {
    if (s.size() >= len) {
        buf_.append(s.data(), len);
    } else {
        buf_.append(s);
        buf_.append(len - s.size(), '\0');
    }
}

Message MessageWriter::BuildMessage(MessageType type) const {
    return Message(type, buf_);
}

// --- MessageReader ---

MessageReader::MessageReader(std::string payload) : payload_(std::move(payload)) {}

char MessageReader::ReadByte() {
    if (pos_ >= payload_.size()) {
        ereport(mytoydb::error::LogLevel::kError, "protocol message: unexpected end of data");
    }
    return payload_[pos_++];
}

std::string MessageReader::ReadBytes(std::size_t len) {
    if (pos_ + len > payload_.size()) {
        ereport(mytoydb::error::LogLevel::kError, "protocol message: unexpected end of data");
    }
    std::string result = payload_.substr(pos_, len);
    pos_ += len;
    return result;
}

int16_t MessageReader::ReadInt16() {
    if (pos_ + 2 > payload_.size()) {
        ereport(mytoydb::error::LogLevel::kError, "protocol message: unexpected end of data");
    }
    uint16_t u =
        (static_cast<uint8_t>(payload_[pos_]) << 8) | static_cast<uint8_t>(payload_[pos_ + 1]);
    pos_ += 2;
    return static_cast<int16_t>(u);
}

int32_t MessageReader::ReadInt32() {
    if (pos_ + 4 > payload_.size()) {
        ereport(mytoydb::error::LogLevel::kError, "protocol message: unexpected end of data");
    }
    uint32_t u = (static_cast<uint8_t>(payload_[pos_]) << 24) |
                 (static_cast<uint8_t>(payload_[pos_ + 1]) << 16) |
                 (static_cast<uint8_t>(payload_[pos_ + 2]) << 8) |
                 static_cast<uint8_t>(payload_[pos_ + 3]);
    pos_ += 4;
    return static_cast<int32_t>(u);
}

std::string MessageReader::ReadString() {
    std::size_t nul = payload_.find('\0', pos_);
    if (nul == std::string::npos) {
        ereport(mytoydb::error::LogLevel::kError,
                "protocol message: missing null terminator in string");
    }
    std::string result = payload_.substr(pos_, nul - pos_);
    pos_ = nul + 1;
    return result;
}

// --- Convenience message builders ---

Message BuildRowDescription(const std::vector<RowDescriptionField>& fields) {
    MessageWriter w;
    w.WriteInt16(static_cast<int16_t>(fields.size()));
    for (const auto& f : fields) {
        w.WriteString(f.name);
        w.WriteInt32(f.table_oid);
        w.WriteInt16(f.column_attnum);
        w.WriteInt32(f.type_oid);
        w.WriteInt16(f.type_size);
        w.WriteInt32(f.type_mod);
        w.WriteInt16(f.format);
    }
    return Message(MessageType::kRowDescription, w.data());
}

Message BuildDataRow(const std::vector<std::string>& values, const std::vector<bool>& isnull) {
    MessageWriter w;
    w.WriteInt16(static_cast<int16_t>(values.size()));
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i < isnull.size() && isnull[i]) {
            w.WriteInt32(-1);
        } else {
            const std::string& v = values[i];
            w.WriteInt32(static_cast<int32_t>(v.size()));
            w.WriteBytes(v.data(), v.size());
        }
    }
    return Message(MessageType::kDataRow, w.data());
}

Message BuildCommandComplete(const std::string& tag) {
    MessageWriter w;
    w.WriteString(tag);
    return Message(MessageType::kCommandComplete, w.data());
}

Message BuildEmptyQueryResponse() {
    return Message(MessageType::kEmptyQueryResponse, "");
}

Message BuildReadyForQuery(TransactionStatus status) {
    MessageWriter w;
    w.WriteByte(static_cast<char>(status));
    return Message(MessageType::kReadyForQuery, w.data());
}

Message BuildErrorResponse(const std::string& message) {
    MessageWriter w;
    // Each field is: field-type (1 byte) + field-value (null-terminated string).
    // Severity field.
    w.WriteByte('S');
    w.WriteString("ERROR");
    // Error code field (SQLSTATE). "XX000" = internal error.
    w.WriteByte('C');
    w.WriteString("XX000");
    // Message field.
    w.WriteByte('M');
    w.WriteString(message);
    // Terminator.
    w.WriteByte('\0');
    return Message(MessageType::kErrorResponse, w.data());
}

Message BuildParseComplete() {
    return Message(MessageType::kParseComplete, "");
}

Message BuildBindComplete() {
    return Message(MessageType::kBindComplete, "");
}

Message BuildCloseComplete() {
    return Message(MessageType::kCloseComplete, "");
}

Message BuildNoData() {
    return Message(MessageType::kNoData, "");
}

}  // namespace mytoydb::protocol
