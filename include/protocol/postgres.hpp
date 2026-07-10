// postgres.h — Frontend protocol handler (simple and extended query).
//
// Converted from PostgreSQL 15's src/backend/tcop/postgres.c.
//
// This module orchestrates the full query processing pipeline:
//   raw_parser() -> parse_analyze() -> planner() -> ExecutorStart/Run/Finish/End
//
// Two protocol modes are supported:
//
// 1. Simple Query Protocol (exec_simple_query):
//    Client sends a single 'Q' message containing one or more SQL statements
//    (separated by semicolons). The server processes each statement and sends
//    RowDescription + DataRows + CommandComplete for SELECTs, or just
//    CommandComplete for DML. A ReadyForQuery message is sent at the end.
//
// 2. Extended Query Protocol:
//    Parse  -> parse SQL into a named prepared statement
//    Bind   -> bind parameters to create a named portal
//    Describe -> describe a statement or portal (RowDescription / NoData)
//    Execute -> execute a portal, returning rows up to a limit
//    Sync   -> send ReadyForQuery (ends an extended query transaction)
//
// The Backend class holds session state: prepared statements, portals, and
// the output sink. It assumes the global catalog, transaction system, and
// storage layer are already initialized by the caller.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"
#include "protocol/pqformat.hpp"

namespace pgcpp::parser {
class Query;
}  // namespace pgcpp::parser

namespace pgcpp::executor {
struct Plan;
}  // namespace pgcpp::executor

namespace pgcpp::server {
class SocketSink;
}  // namespace pgcpp::server

namespace pgcpp::utils {
class CachedPlanSource;
}  // namespace pgcpp::utils

namespace pgcpp::protocol {

// PreparedStatement — a parsed statement in the extended query protocol.
//
// A single Parse message may contain multiple SQL statements separated by
// semicolons; each produces one Query. Only the first Query's parameter
// types are tracked (PostgreSQL allows parameters only in the first
// statement of a multi-statement parse).
struct PreparedStatement {
    std::string name;
    std::vector<pgcpp::parser::Query*> queries;
    std::vector<pgcpp::utils::CachedPlanSource*> plan_sources;
    std::vector<pgcpp::catalog::Oid> param_types;
    bool has_results = false;  // true if the first query is a SELECT

    ~PreparedStatement();
};

// Portal — a bound, ready-to-execute statement.
struct Portal {
    std::string name;
    PreparedStatement* stmt = nullptr;
    int query_index = 0;                    // which query in the prepared statement to run
    std::vector<std::string> param_values;  // bound parameter values (text)
    std::vector<bool> param_isnull;
};

// Backend — session state for the frontend protocol.
//
// Holds prepared statements, portals, and the output sink. The caller is
// responsible for initializing the global catalog, transaction system, and
// storage layer before using this class.
class Backend {
public:
    explicit Backend(OutputSink* sink);
    ~Backend();

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    // --- Simple Query Protocol ---

    // exec_simple_query — process a simple query string.
    //
    // Parses, analyzes, plans, and executes each statement in the query
    // string. For SELECT statements, sends RowDescription ('T'), one DataRow
    // ('D') per result row, and CommandComplete ('C'). For DML, sends only
    // CommandComplete. For empty queries, sends EmptyQueryResponse ('I').
    // Finally sends ReadyForQuery ('Z') with the current transaction status.
    //
    // On error (ereport), sends an ErrorResponse ('E') and aborts the
    // current transaction. The caller is responsible for transaction state
    // cleanup.
    void exec_simple_query(const std::string& query_string);

    // --- Extended Query Protocol ---

    // HandleParse — parse SQL into a named prepared statement.
    // Sends ParseComplete ('1') on success, ErrorResponse ('E') on failure.
    void HandleParse(const std::string& stmt_name, const std::string& query,
                     const std::vector<pgcpp::catalog::Oid>& param_types);

    // HandleBind — bind parameters to a prepared statement, creating a portal.
    // Sends BindComplete ('2') on success, ErrorResponse ('E') on failure.
    void HandleBind(const std::string& portal_name, const std::string& stmt_name,
                    const std::vector<std::string>& param_values,
                    const std::vector<bool>& param_isnull);

    // HandleDescribe — describe a statement ('S') or portal ('P').
    // For a statement, sends ParameterDescription ('t') followed by
    // RowDescription ('T') or NoData ('n').
    // For a portal, sends RowDescription ('T') or NoData ('n').
    void HandleDescribe(DescribeKind kind, const std::string& name);

    // HandleExecute — execute a portal.
    // Sends RowDescription ('T') on the first row (if not already described),
    // DataRow ('D') per row (up to max_rows), then CommandComplete ('C') or
    // PortalSuspended ('s') if max_rows was reached.
    void HandleExecute(const std::string& portal_name, int max_rows);

    // HandleSync — send ReadyForQuery ('Z') and commit the transaction.
    void HandleSync();

    // HandleFlush — flush the output sink (no ReadyForQuery).
    void HandleFlush();

    // HandleClose — close a prepared statement or portal.
    // Sends CloseComplete ('3').
    void HandleClose(DescribeKind kind, const std::string& name);

    // --- Accessors for testing ---

    PreparedStatement* FindPreparedStatement(const std::string& name) const;
    Portal* FindPortal(const std::string& name) const;

    OutputSink* sink() const { return sink_; }

private:
    // Execute a single analyzed Query and send result messages.
    // Returns the command tag (e.g., "SELECT 3", "INSERT 0 1").
    // If plan is nullptr, plans the query internally (simple query path).
    // If plan is provided (from the plan cache), uses it directly.
    // If send_row_description is true, sends RowDescription before DataRows
    // (used by the simple query protocol; the extended query protocol sends
    // RowDescription via Describe instead).
    std::string ExecuteQuery(pgcpp::parser::Query* query, pgcpp::executor::Plan* plan = nullptr,
                             bool send_row_description = true);

    // Send a RowDescription for a SELECT query's target list.
    void SendRowDescription(pgcpp::parser::Query* query);

    // Send a DataRow from a TupleTableSlot.
    // Encodes each attribute using the type's output function.
    void SendDataRow(pgcpp::parser::Query* query, const std::vector<std::string>& values,
                     const std::vector<bool>& isnull);

    // Get the current transaction status for ReadyForQuery.
    TransactionStatus GetCurrentTransactionStatus() const;

    // Send an error response and record that an error occurred.
    void SendError(const std::string& message);

    OutputSink* sink_;
    std::map<std::string, PreparedStatement*> prepared_statements_;
    std::map<std::string, Portal*> portals_;

    // The unnamed prepared statement and portal are stored with empty names
    // and are always replaced (never explicitly closed).
};

// PostgresMain — full per-connection server main loop.
//
// This is the PG-style entry point (src/backend/tcop/postgres.c). It handles:
//   - reading messages from the client socket
//   - dispatching to a Backend handler
//   - implicit transaction management (one transaction per command outside a
//     BEGIN/COMMIT block)
//   - error recovery (ereport ERROR -> ErrorResponse -> ReadyForQuery)
//   - COPY message dispatch stubs
//
// `client_fd` is the accepted TCP socket. `sink` is the SocketSink wrapping it.
// The caller (postmaster) is responsible for fork() and resource init.
void PostgresMain(int client_fd, pgcpp::server::SocketSink* sink);

// ProcessInterrupts — called at safe points in the main loop.
// If the interrupt-pending flag is set, ereport(ERROR) with a cancel message.
void ProcessInterrupts();

// SetInterruptPending — set the interrupt-pending flag (signal-safe).
void SetInterruptPending();

}  // namespace pgcpp::protocol
