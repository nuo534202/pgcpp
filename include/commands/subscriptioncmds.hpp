// subscriptioncmds.h — CREATE / ALTER / DROP SUBSCRIPTION (P3-13 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/subscriptioncmds.c.
//
// Subscriptions consume a publication on a remote server via a replication
// connection. CREATE SUBSCRIPTION inserts a row in pg_subscription, opens
// the connection, and starts the apply worker.
//
// pgcpp's skeleton implementation parses the statements and dispatches to
// these handlers. The replication worker lands in P3-4 (logical
// replication); until then the handlers validate the parse tree and return
// the command tag.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateSubscriptionStmt;
class AlterSubscriptionStmt;
class DropSubscriptionStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// CreateSubscription — execute CREATE SUBSCRIPTION.
std::string CreateSubscription(parser::CreateSubscriptionStmt* stmt);

// AlterSubscription — execute ALTER SUBSCRIPTION.
std::string AlterSubscription(parser::AlterSubscriptionStmt* stmt);

// DropSubscription — execute DROP SUBSCRIPTION.
std::string DropSubscription(parser::DropSubscriptionStmt* stmt);

}  // namespace pgcpp::commands
