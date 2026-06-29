// dbcommands.cpp — CREATE/DROP DATABASE implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/dbcommands.c.
// pgcpp is a single-database instance; these are stubs that
// acknowledge the request without actually creating/dropping a
// database. (A real implementation would fork+exec initdb or copy a
// template database.)
#include "commands/dbcommands.hpp"

#include <string>

#include "parser/parsenodes.hpp"

namespace pgcpp::commands {

using pgcpp::parser::CreatedbStmt;
using pgcpp::parser::DropdbStmt;

std::string createdb(CreatedbStmt* stmt) {
    if (stmt == nullptr)
        return "CREATE DATABASE";
    // Stub: real PostgreSQL forks a subprocess to copy template1.
    // pgcpp runs as a single fixed database ("pgcpp").
    return "CREATE DATABASE";
}

std::string dropdb(DropdbStmt* stmt) {
    if (stmt == nullptr)
        return "DROP DATABASE";
    if (stmt->missing_ok)
        return "DROP DATABASE";
    return "DROP DATABASE";
}

}  // namespace pgcpp::commands
