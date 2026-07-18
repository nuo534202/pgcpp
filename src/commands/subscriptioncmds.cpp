// subscriptioncmds.cpp — CREATE / ALTER / DROP SUBSCRIPTION implementation (P3-13).
//
// Converted from PostgreSQL 15's src/backend/commands/subscriptioncmds.c.
//
// Skeleton implementation: validates the parse tree and returns the command
// tag. Replication worker integration lands in P3-4.
#include "commands/subscriptioncmds.hpp"

#include <string>

#include "common/error/elog.hpp"
#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::AlterSubscriptionStmt;
using pgcpp::parser::CreateSubscriptionStmt;
using pgcpp::parser::DropSubscriptionStmt;

std::string CreateSubscription(CreateSubscriptionStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE SUBSCRIPTION: null statement");
    }
    if (stmt->subname.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE SUBSCRIPTION: missing subscription name");
    }
    if (stmt->conninfo.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE SUBSCRIPTION: missing CONNECTION string");
    }
    if (stmt->publications.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "CREATE SUBSCRIPTION: missing PUBLICATION name");
    }
    // pg_subscription insert and replication worker start land in P3-4.
    return "CREATE SUBSCRIPTION";
}

std::string AlterSubscription(AlterSubscriptionStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER SUBSCRIPTION: null statement");
    }
    if (stmt->subname.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "ALTER SUBSCRIPTION: missing subscription name");
    }
    return "ALTER SUBSCRIPTION";
}

std::string DropSubscription(DropSubscriptionStmt* stmt) {
    if (stmt == nullptr) {
        ereport(pgcpp::error::LogLevel::kError, "DROP SUBSCRIPTION: null statement");
    }
    if (stmt->subname.empty()) {
        ereport(pgcpp::error::LogLevel::kError, "DROP SUBSCRIPTION: missing subscription name");
    }
    // Replication slot cleanup and worker stop land in P3-4.
    return "DROP SUBSCRIPTION";
}

}  // namespace pgcpp::commands
