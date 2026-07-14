// foreign.h — FDW catalog: foreign servers, user mappings, foreign tables.
//
// Converted from PostgreSQL 15's src/include/foreign/foreign.h and
// src/backend/foreign/foreign.c.
//
// In PostgreSQL these are stored in the pg_foreign_server, pg_user_mapping,
// and pg_foreign_table catalog tables. pgcpp keeps an in-memory store
// (following the twophase / replication-slot pattern) with the same row
// types and lookup semantics.
//
// A foreign table is also a pg_class row (relkind = 'f'). The FDW catalog
// links the pg_class OID to a foreign server and per-table options.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "catalog/catalog.hpp"

namespace pgcpp::foreign {

// FdwOption — a name/value pair for foreign server, user mapping, or
// foreign table options (e.g., {"filename", "/tmp/data.csv"}).
struct FdwOption {
    std::string optname;
    std::string optvalue;
};

// ForeignServer — corresponds to a pg_foreign_server row.
struct ForeignServer {
    pgcpp::catalog::Oid serverid = pgcpp::catalog::kInvalidOid;
    std::string servername;
    std::string fdwname;  // FDW handler name (e.g., "file_fdw")
    pgcpp::catalog::Oid owner = pgcpp::catalog::kInvalidOid;
    std::vector<FdwOption> options;
};

// UserMapping — corresponds to a pg_user_mapping row.
struct UserMapping {
    pgcpp::catalog::Oid umid = pgcpp::catalog::kInvalidOid;
    pgcpp::catalog::Oid serverid = pgcpp::catalog::kInvalidOid;
    pgcpp::catalog::Oid userid = pgcpp::catalog::kInvalidOid;
    std::vector<FdwOption> options;
};

// ForeignTable — corresponds to a pg_foreign_table row.
struct ForeignTable {
    pgcpp::catalog::Oid relid = pgcpp::catalog::kInvalidOid;  // pg_class OID
    pgcpp::catalog::Oid serverid = pgcpp::catalog::kInvalidOid;
    std::vector<FdwOption> options;
};

// --- Foreign server API ---

// CreateForeignServer — register a foreign server. Returns the assigned OID.
// Throws ereport(ERROR) if the server name already exists.
pgcpp::catalog::Oid CreateForeignServer(const std::string& servername, const std::string& fdwname,
                                        pgcpp::catalog::Oid owner = pgcpp::catalog::kInvalidOid,
                                        const std::vector<FdwOption>& options = {});

// LookupForeignServerByName — return the foreign server with the given name,
// or nullptr if not found.
const ForeignServer* LookupForeignServerByName(const std::string& servername);

// LookupForeignServerByOid — return the foreign server with the given OID,
// or nullptr if not found.
const ForeignServer* LookupForeignServerByOid(pgcpp::catalog::Oid serverid);

// DropForeignServer — remove a foreign server by OID. Returns false if not
// found. Also removes all user mappings and foreign tables referencing it.
bool DropForeignServer(pgcpp::catalog::Oid serverid);

// NumForeignServers — return the number of registered foreign servers.
std::size_t NumForeignServers();

// --- User mapping API ---

// CreateUserMapping — register a user mapping. Returns the assigned OID.
// Throws ereport(ERROR) if a mapping already exists for (serverid, userid).
pgcpp::catalog::Oid CreateUserMapping(pgcpp::catalog::Oid serverid, pgcpp::catalog::Oid userid,
                                      const std::vector<FdwOption>& options = {});

// LookupUserMapping — return the user mapping for (serverid, userid), or
// nullptr if not found.
const UserMapping* LookupUserMapping(pgcpp::catalog::Oid serverid, pgcpp::catalog::Oid userid);

// DropUserMapping — remove a user mapping by OID. Returns false if not found.
bool DropUserMapping(pgcpp::catalog::Oid umid);

// NumUserMappings — return the number of registered user mappings.
std::size_t NumUserMappings();

// --- Foreign table API ---

// CreateForeignTable — register a foreign table. The relid must correspond
// to an existing pg_class row with relkind = 'f'. Returns the relid.
// Throws ereport(ERROR) if a foreign table with this relid already exists
// or if the serverid does not refer to a valid foreign server.
pgcpp::catalog::Oid CreateForeignTable(pgcpp::catalog::Oid relid, pgcpp::catalog::Oid serverid,
                                       const std::vector<FdwOption>& options = {});

// LookupForeignTable — return the foreign table with the given relid, or
// nullptr if not found.
const ForeignTable* LookupForeignTable(pgcpp::catalog::Oid relid);

// DropForeignTable — remove a foreign table by relid. Returns false if not
// found.
bool DropForeignTable(pgcpp::catalog::Oid relid);

// NumForeignTables — return the number of registered foreign tables.
std::size_t NumForeignTables();

// --- Reset (for testing) ---

// ResetForeignCatalog — clear all foreign servers, user mappings, and
// foreign tables from the in-memory store.
void ResetForeignCatalog();

// --- Option helpers ---

// GetOption — find an option by name. Returns nullptr if not present.
const std::string* GetOption(const std::vector<FdwOption>& options, const std::string& name);

}  // namespace pgcpp::foreign
