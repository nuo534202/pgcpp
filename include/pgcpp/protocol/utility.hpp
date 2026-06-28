// utility.h — Utility command dispatch (ProcessUtility).
//
// Converted from PostgreSQL 15's src/backend/tcop/utility.c.
//
// ProcessUtility executes non-SELECT/DML statements (CREATE, DROP, ALTER,
// TRUNCATE, VACUUM, COPY, SET, BEGIN/COMMIT/ROLLBACK, etc.). It dispatches
// on the parse-tree node type to the appropriate handler, mutating the
// catalog and storage layer as needed.
//
// On error, handlers call ereport(ERROR); the caller is expected to wrap the
// call in PG_TRY/PG_CATCH to convert the error into an ErrorResponse message.
#pragma once

#include <string>

#include "pgcpp/catalog/catalog.hpp"
#include "pgcpp/common/containers/node.hpp"
#include "pgcpp/protocol/pqformat.hpp"

namespace pgcpp::protocol {

// ProcessUtility — execute a utility (non-SELECT/DML) statement.
// Dispatches on the node type to the appropriate handler.
// Returns the command tag (e.g., "DROP TABLE", "CREATE INDEX").
// On error, ereport(ERROR) is called (caller should PG_TRY/PG_CATCH).
std::string ProcessUtility(pgcpp::nodes::Node* stmt, OutputSink* sink);

// CreateCommandTag — return the canonical command tag for a utility statement.
// Used for protocol CommandComplete messages and logging.
std::string CreateCommandTag(pgcpp::nodes::Node* stmt);

// UtilityReturnsTuples — true if the utility returns rows (e.g., EXPLAIN,
// VACUUM VERBOSE). Simplified: returns false for all utility statements.
bool UtilityReturnsTuples(pgcpp::nodes::Node* stmt);

}  // namespace pgcpp::protocol
