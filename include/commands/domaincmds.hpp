// domaincmds.h — CREATE DOMAIN (M14 commands module).
//
// Converted from PostgreSQL 15's src/backend/commands/domaincmds.c.
#pragma once

#include <string>

namespace pgcpp::parser {
class CreateDomainStmt;
}  // namespace pgcpp::parser

namespace pgcpp::commands {

// DefineDomain — execute CREATE DOMAIN.
// Registers a pg_type row with typtype=kDomain and typbasetype set
// to the base type's OID.
std::string DefineDomain(parser::CreateDomainStmt* stmt);

}  // namespace pgcpp::commands
