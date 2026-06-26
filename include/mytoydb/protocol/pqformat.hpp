// pqformat.h — PostgreSQL frontend/backend protocol message encoding and decoding.
//
// Converted from PostgreSQL 15's src/backend/libpq/pqformat.c.
//
// The PostgreSQL wire protocol uses a simple framed format:
//   - 1 byte:  message type (ASCII character)
//   - 4 bytes: message length (network byte order, includes self but not type)
//   - N bytes: payload
//
// This file provides:
//   - Message:           a parsed/constructed protocol message (type + payload)
//   - MessageWriter:     builds payloads by appending bytes/ints/strings
//   - MessageReader:     parses payloads by reading bytes/ints/strings
//   - OutputSink:        abstract interface for sending messages to a client
//   - StringSink:        test sink that collects messages into a vector
//
// All multi-byte integers use network byte order (big-endian), matching
// PostgreSQL's wire format.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mytoydb::protocol {

// MessageType — ASCII message type codes used in the wire protocol.
//
// Some characters are overloaded between client->server and server->client
// directions (e.g., 'D' is Describe from client but DataRow from server).
// The direction disambiguates which meaning applies.
enum class MessageType : char {
    // --- Frontend (client) -> Backend (server) ---
    kQuery = 'Q',            // Simple query
    kParse = 'P',            // Extended query: Parse
    kBind = 'B',             // Extended query: Bind
    kDescribe = 'D',         // Extended query: Describe
    kExecute = 'E',          // Extended query: Execute
    kSync = 'S',             // Extended query: Sync
    kClose = 'C',            // Extended query: Close
    kTerminate = 'X',        // Terminate connection
    kFlush = 'H',            // Flush pending output
    kCopyData = 'd',         // COPY data
    kCopyDone = 'c',         // COPY done
    kCopyFail = 'f',         // COPY fail
    kPasswordMessage = 'p',  // Password / SASL response

    // --- Backend (server) -> Frontend (client) ---
    kAuthentication = 'R',        // Authentication request
    kParamStatus = 'S',           // Parameter status (overloaded with Sync)
    kBackendKeyData = 'K',        // Backend key data
    kReadyForQuery = 'Z',         // Ready for query
    kRowDescription = 'T',        // Row description
    kDataRow = 'D',               // Data row (overloaded with Describe)
    kCommandComplete = 'C',       // Command complete (overloaded with Close)
    kEmptyQueryResponse = 'I',    // Empty query response
    kErrorResponse = 'E',         // Error response (overloaded with Execute)
    kNoticeResponse = 'N',        // Notice response
    kParseComplete = '1',         // Parse complete
    kBindComplete = '2',          // Bind complete
    kCloseComplete = '3',         // Close complete
    kPortalSuspended = 's',       // Portal suspended (execute limit reached)
    kNoData = 'n',                // No data (describe returned no rows)
    kParameterDescription = 't',  // Parameter description
    kNotify = 'A',                // Asynchronous notification
};

// TransactionStatus — transaction status indicator sent in ReadyForQuery.
enum class TransactionStatus : char {
    kIdle = 'I',                 // Not in a transaction
    kInTransaction = 'T',        // In a transaction block
    kInFailedTransaction = 'E',  // In a failed transaction block
};

// ErrorField — field codes for ErrorResponse / NoticeResponse messages.
enum class ErrorField : char {
    kSeverity = 'S',
    kSeverityNonlocalized = 'V',
    kCode = 'C',
    kMessage = 'M',
    kDetail = 'D',
    kHint = 'H',
    kPosition = 'P',
    kInternalPosition = 'p',
    kInternalQuery = 'q',
    kWhere = 'W',
    kSchema = 's',
    kTable = 't',
    kColumn = 'c',
    kDatatype = 'd',
    kConstraint = 'n',
    kFile = 'F',
    kLine = 'L',
    kRoutine = 'R',
};

// DescribeKind — the kind of object to describe in a Describe message.
enum class DescribeKind : char {
    kStatement = 'S',
    kPortal = 'P',
};

// Message — a single protocol message (type + payload bytes).
//
// The payload excludes the type byte and the 4-byte length prefix.
// Use BuildWireFormat() to produce the full on-wire bytes.
struct Message {
    MessageType type = MessageType::kReadyForQuery;
    std::string payload;

    Message() = default;
    Message(MessageType t, std::string p) : type(t), payload(std::move(p)) {}

    // Build the full on-wire representation: type byte + 4-byte length + payload.
    // The length field includes itself (4 bytes) but not the type byte.
    std::string BuildWireFormat() const;
};

// MessageWriter — builds message payloads using PostgreSQL's wire format.
//
// All multi-byte integers are written in network byte order (big-endian).
// Strings are written as null-terminated C strings unless otherwise noted.
class MessageWriter {
public:
    MessageWriter() = default;

    // Append a single byte.
    void WriteByte(char b);
    // Append raw bytes.
    void WriteBytes(const char* data, std::size_t len);
    // Append a 16-bit signed integer (network byte order).
    void WriteInt16(int16_t v);
    // Append a 32-bit signed integer (network byte order).
    void WriteInt32(int32_t v);
    // Append a null-terminated string (including the trailing NUL).
    void WriteString(const std::string& s);
    // Append a string of exactly len bytes (no NUL terminator).
    void WriteStringN(const std::string& s, std::size_t len);

    const std::string& data() const { return buf_; }
    std::string& data() { return buf_; }
    void clear() { buf_.clear(); }

    // Build a complete Message with the given type and the current buffer as
    // payload. Does not clear the writer.
    Message BuildMessage(MessageType type) const;

private:
    std::string buf_;
};

// MessageReader — parses message payloads.
//
// Reads advance an internal position; attempting to read past the end
// calls ereport(ERROR).
class MessageReader {
public:
    explicit MessageReader(std::string payload);

    // Read a single byte.
    char ReadByte();
    // Read len bytes as a string.
    std::string ReadBytes(std::size_t len);
    // Read a 16-bit signed integer (network byte order).
    int16_t ReadInt16();
    // Read a 32-bit signed integer (network byte order).
    int32_t ReadInt32();
    // Read a null-terminated string (the NUL is consumed but not returned).
    std::string ReadString();

    bool eof() const { return pos_ >= payload_.size(); }
    std::size_t remaining() const { return payload_.size() - pos_; }
    std::size_t length() const { return payload_.size(); }

private:
    std::string payload_;
    std::size_t pos_ = 0;
};

// OutputSink — abstract interface for sending messages to a client.
//
// In a real server, the implementation writes to a socket. In tests,
// StringSink collects messages for inspection.
class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void SendMessage(const Message& msg) = 0;
};

// StringSink — collects all sent messages into a vector (for testing).
class StringSink : public OutputSink {
public:
    void SendMessage(const Message& msg) override { messages_.push_back(msg); }
    const std::vector<Message>& messages() const { return messages_; }
    std::vector<Message>& messages() { return messages_; }
    void clear() { messages_.clear(); }
    std::size_t size() const { return messages_.size(); }
    const Message& at(std::size_t i) const { return messages_.at(i); }

private:
    std::vector<Message> messages_;
};

// --- Convenience message builders ---

// Build a RowDescription message from column metadata.
// Each column is described by: name (string), table OID (int32), column attnum
// (int16), type OID (int32), type size (int16), type modifier (int32),
// format code (int16, 0=text).
struct RowDescriptionField {
    std::string name;           // column name
    int32_t table_oid = 0;      // OID of the source table (0 if not a column)
    int16_t column_attnum = 0;  // column number (0 if not a column)
    int32_t type_oid = 0;       // OID of the column's data type
    int16_t type_size = 0;      // typlen from pg_type (-1 for varlena)
    int32_t type_mod = -1;      // typmod from pg_attribute (-1 if none)
    int16_t format = 0;         // 0 = text, 1 = binary
};

Message BuildRowDescription(const std::vector<RowDescriptionField>& fields);

// Build a DataRow message from text-encoded values.
// Each value is a string; null values are indicated by isnull.
Message BuildDataRow(const std::vector<std::string>& values, const std::vector<bool>& isnull);

// Build a CommandComplete message with a command tag (e.g., "SELECT 3").
Message BuildCommandComplete(const std::string& tag);

// Build an EmptyQueryResponse message.
Message BuildEmptyQueryResponse();

// Build a ReadyForQuery message with the given transaction status.
Message BuildReadyForQuery(TransactionStatus status);

// Build an ErrorResponse message from an error message string.
Message BuildErrorResponse(const std::string& message);

// Build a ParseComplete / BindComplete / CloseComplete / NoData message.
Message BuildParseComplete();
Message BuildBindComplete();
Message BuildCloseComplete();
Message BuildNoData();

}  // namespace mytoydb::protocol
