// psql_client.cpp — PsqlClient implementation.
//
// Implements the TCP connection, startup handshake, and simple query
// protocol for the pgcpp client.
#include "pgcpp/tools/psql_client.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <sstream>
#include <string>

namespace pgcpp::tools {

namespace {

// Write all bytes to a fd (handles partial writes).
bool WriteAll(int fd, const void* data, size_t len) {
    const char* p = static_cast<const char*>(data);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        written += static_cast<size_t>(n);
    }
    return true;
}

// Read exactly len bytes from a fd (handles partial reads).
bool ReadAll(int fd, void* buf, size_t len) {
    char* p = static_cast<char*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;  // EOF
        got += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// PsqlClient
// ---------------------------------------------------------------------------

PsqlClient::PsqlClient(const std::string& host, int port) : host_(host), port_(port), fd_(-1) {}

PsqlClient::~PsqlClient() {
    Disconnect();
}

bool PsqlClient::Connect() {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0)
        return false;

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    if (connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    // Set a read timeout.
    struct timeval tv;
    tv.tv_sec = 10;
    tv.tv_usec = 0;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (!SendStartupMessage()) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    if (!ReadStartupResponse()) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    return true;
}

bool PsqlClient::SendStartupMessage() {
    // Startup message: length (4) + protocol version (4) + "user\0pgcpp\0\0"
    std::string payload;
    int32_t proto = htonl(0x00030000);
    payload.append(reinterpret_cast<const char*>(&proto), 4);
    payload.append("user\0pgcpp\0", 12);
    payload.push_back('\0');

    int32_t len = htonl(static_cast<int32_t>(4 + payload.size()));
    std::string msg(reinterpret_cast<const char*>(&len), 4);
    msg += payload;
    return WriteAll(fd_, msg.data(), msg.size());
}

bool PsqlClient::ReadStartupResponse() {
    while (true) {
        char type;
        std::string payload;
        if (!ReadMessage(&type, &payload))
            return false;

        switch (type) {
            case 'R':  // AuthenticationOk
                break;
            case 'S':  // ParameterStatus
                break;
            case 'K':  // BackendKeyData
                break;
            case 'Z':  // ReadyForQuery
                return true;
            case 'E': {  // ErrorResponse
                return false;
            }
            default:
                break;
        }
    }
}

bool PsqlClient::ReadMessage(char* type, std::string* payload) {
    if (!ReadAll(fd_, type, 1))
        return false;

    char len_buf[4];
    if (!ReadAll(fd_, len_buf, 4))
        return false;

    int32_t length = static_cast<int32_t>(
        (static_cast<uint8_t>(len_buf[0]) << 24) | (static_cast<uint8_t>(len_buf[1]) << 16) |
        (static_cast<uint8_t>(len_buf[2]) << 8) | static_cast<uint8_t>(len_buf[3]));

    if (length < 4)
        return false;

    size_t payload_len = static_cast<size_t>(length) - 4;
    if (payload_len > 0) {
        payload->resize(payload_len);
        if (!ReadAll(fd_, payload->data(), payload_len))
            return false;
    } else {
        payload->clear();
    }

    return true;
}

QueryResult PsqlClient::ExecuteQuery(const std::string& query) {
    QueryResult result;

    if (fd_ < 0) {
        result.error_message = "not connected";
        return result;
    }

    // Send the 'Q' (Simple Query) message.
    std::string payload = query;
    payload.push_back('\0');  // null-terminated

    std::string msg;
    msg.push_back('Q');
    int32_t len = htonl(static_cast<int32_t>(4 + payload.size()));
    msg.append(reinterpret_cast<const char*>(&len), 4);
    msg += payload;

    if (!WriteAll(fd_, msg.data(), msg.size())) {
        result.error_message = "failed to send query";
        close(fd_);
        fd_ = -1;
        return result;
    }

    // Read response messages until ReadyForQuery.
    while (true) {
        char type;
        std::string payload;
        if (!ReadMessage(&type, &payload)) {
            result.error_message = "connection closed by server";
            close(fd_);
            fd_ = -1;
            return result;
        }

        switch (type) {
            case 'T': {  // RowDescription
                // Parse column names.
                if (payload.size() >= 2) {
                    int16_t nfields = static_cast<int16_t>((static_cast<uint8_t>(payload[0]) << 8) |
                                                           static_cast<uint8_t>(payload[1]));
                    size_t pos = 2;
                    for (int16_t i = 0; i < nfields && pos < payload.size(); ++i) {
                        size_t end = payload.find('\0', pos);
                        if (end == std::string::npos)
                            break;
                        result.column_names.push_back(payload.substr(pos, end - pos));
                        pos = end + 1;
                        // Skip: table_oid(4) + attnum(2) + type_oid(4) + type_size(2) +
                        //       type_mod(4) + format(2) = 18 bytes
                        pos += 18;
                    }
                }
                break;
            }
            case 'D': {  // DataRow
                if (payload.size() >= 2) {
                    int16_t ncols = static_cast<int16_t>((static_cast<uint8_t>(payload[0]) << 8) |
                                                         static_cast<uint8_t>(payload[1]));
                    std::vector<std::string> row;
                    size_t pos = 2;
                    for (int16_t i = 0; i < ncols; ++i) {
                        if (pos + 4 > payload.size())
                            break;
                        int32_t col_len =
                            static_cast<int32_t>((static_cast<uint8_t>(payload[pos]) << 24) |
                                                 (static_cast<uint8_t>(payload[pos + 1]) << 16) |
                                                 (static_cast<uint8_t>(payload[pos + 2]) << 8) |
                                                 static_cast<uint8_t>(payload[pos + 3]));
                        pos += 4;
                        if (col_len < 0) {
                            // NULL value
                            row.emplace_back("");
                        } else {
                            if (pos + static_cast<size_t>(col_len) > payload.size())
                                break;
                            row.emplace_back(payload.substr(pos, static_cast<size_t>(col_len)));
                            pos += static_cast<size_t>(col_len);
                        }
                    }
                    result.rows.push_back(std::move(row));
                }
                break;
            }
            case 'C': {  // CommandComplete
                // The payload is a null-terminated command tag string.
                size_t end = payload.find('\0');
                if (end != std::string::npos) {
                    result.command_tag = payload.substr(0, end);
                } else {
                    result.command_tag = payload;
                }
                result.success = true;
                break;
            }
            case 'I': {  // EmptyQueryResponse
                result.command_tag = "";
                result.success = true;
                break;
            }
            case 'E': {  // ErrorResponse
                // Parse the error message field ('M').
                for (size_t i = 0; i < payload.size();) {
                    char field = payload[i];
                    if (field == '\0')
                        break;
                    ++i;
                    size_t end = payload.find('\0', i);
                    if (end == std::string::npos)
                        break;
                    if (field == 'M') {
                        result.error_message = payload.substr(i, end - i);
                    }
                    i = end + 1;
                }
                result.success = false;
                break;
            }
            case 'Z':  // ReadyForQuery
                // Done reading responses.
                return result;
            case 'N':  // NoticeResponse — ignore
                break;
            default:
                break;
        }
    }
}

void PsqlClient::Disconnect() {
    if (fd_ >= 0) {
        // Send Terminate message.
        char msg[5];
        msg[0] = 'X';
        int32_t len = htonl(4);
        std::memcpy(&msg[1], &len, 4);
        WriteAll(fd_, msg, 5);
        close(fd_);
        fd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// Result formatting
// ---------------------------------------------------------------------------

std::string FormatQueryResult(const QueryResult& result) {
    std::ostringstream oss;

    if (!result.success) {
        oss << "ERROR:  " << result.error_message << "\n";
        return oss.str();
    }

    if (result.column_names.empty()) {
        // No result set (DML or utility command).
        if (!result.command_tag.empty()) {
            oss << result.command_tag << "\n";
        }
        return oss.str();
    }

    // Calculate column widths.
    std::vector<size_t> widths(result.column_names.size(), 0);
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        widths[i] = result.column_names[i].size();
    }
    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            if (row[i].size() > widths[i]) {
                widths[i] = row[i].size();
            }
        }
    }

    // Print header.
    oss << " ";
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        if (i > 0)
            oss << " | ";
        oss << result.column_names[i];
        // Pad to width.
        size_t pad = widths[i] - result.column_names[i].size();
        oss << std::string(pad, ' ');
    }
    oss << "\n";

    // Print separator.
    oss << "-";
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        if (i > 0)
            oss << "-+-";
        oss << std::string(widths[i], '-');
    }
    oss << "\n";

    // Print rows.
    for (const auto& row : result.rows) {
        oss << " ";
        for (size_t i = 0; i < result.column_names.size(); ++i) {
            if (i > 0)
                oss << " | ";
            if (i < row.size()) {
                oss << row[i];
                size_t pad = widths[i] - row[i].size();
                oss << std::string(pad, ' ');
            } else {
                oss << std::string(widths[i], ' ');
            }
        }
        oss << "\n";
    }

    // Print row count.
    oss << "(" << result.rows.size() << " row" << (result.rows.size() == 1 ? "" : "s") << ")\n";

    return oss.str();
}

std::string FormatQueryResultCsv(const QueryResult& result) {
    std::ostringstream oss;

    // Header.
    for (size_t i = 0; i < result.column_names.size(); ++i) {
        if (i > 0)
            oss << ",";
        oss << result.column_names[i];
    }
    if (!result.column_names.empty())
        oss << "\n";

    // Rows.
    for (const auto& row : result.rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i > 0)
                oss << ",";
            oss << row[i];
        }
        oss << "\n";
    }

    return oss.str();
}

}  // namespace pgcpp::tools
