// psql_client.h — PsqlClient: a programmatic PostgreSQL-protocol client.
//
// This is the core library behind the `psql` command-line tool (M13).
// It connects to a pgcpp server via TCP, performs the startup handshake,
// and executes queries using the simple query protocol.
//
// The class is designed to be testable: integration tests can start a
// server, create a PsqlClient, execute queries, and inspect the results
// without spawning a subprocess.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::tools {

// QueryResult — the result of executing a query.
struct QueryResult {
    bool success = false;                        // true if no ErrorResponse was received
    std::vector<std::string> column_names;       // column names from RowDescription
    std::vector<std::vector<std::string>> rows;  // data rows (text-encoded values)
    std::string command_tag;                     // e.g., "SELECT 3", "INSERT 0 1"
    std::string error_message;                   // error message if !success
};

// PsqlClient — a TCP client for the pgcpp server.
//
// Usage:
//   PsqlClient client("127.0.0.1", 5433);
//   if (!client.Connect()) { ... error ... }
//   QueryResult result = client.ExecuteQuery("SELECT 1");
//   client.Disconnect();
class PsqlClient {
public:
    PsqlClient(const std::string& host, int port);
    ~PsqlClient();

    PsqlClient(const PsqlClient&) = delete;
    PsqlClient& operator=(const PsqlClient&) = delete;

    // Connect to the server and perform the startup handshake.
    // Returns true on success, false on error.
    bool Connect();

    // Execute a simple query and return the result.
    // Reads all response messages until ReadyForQuery.
    QueryResult ExecuteQuery(const std::string& query);

    // Disconnect from the server (sends Terminate and closes the socket).
    void Disconnect();

    // Check if the client is connected.
    bool IsConnected() const { return fd_ >= 0; }

private:
    // Send the PostgreSQL startup message.
    bool SendStartupMessage();

    // Read and process the startup response (AuthenticationOk,
    // ParameterStatus, BackendKeyData, ReadyForQuery).
    // Returns true on success.
    bool ReadStartupResponse();

    // Read a single protocol message from the server.
    // Returns true if a message was read, false on EOF/error.
    bool ReadMessage(char* type, std::string* payload);

    std::string host_;
    int port_;
    int fd_;
};

// FormatQueryResult — format a QueryResult as a human-readable table string.
// Used by the psql command-line tool to display results.
std::string FormatQueryResult(const QueryResult& result);

// FormatQueryResultCsv — format a QueryResult as CSV.
std::string FormatQueryResultCsv(const QueryResult& result);

}  // namespace pgcpp::tools
