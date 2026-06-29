// psql_large_obj.h — Large-object meta-commands (large_obj.c).
//
// Converted from PostgreSQL 15's src/bin/psql/large_obj.c.
//
// psql provides four backslash commands for managing large objects:
//   \lo_export OID        — write the LO to a client-side file
//   \lo_import FILE [CMT] — read a client-side file into a new LO
//   \lo_list              — list all LOs (SELECT from pg_largeobject_metadata)
//   \lo_unlink OID        — delete an LO
//
// pgcpp implements these as thin wrappers over the server-side lo_*
// fastpath API (exposed via the protocol module). The `\lo_import` and
// `\lo_export` commands also touch the client filesystem.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>

#include "tools/psql_client.hpp"

namespace pgcpp::tools {

// LargeObjectResult — outcome of a large-object meta-command.
enum class LargeObjectResult {
    kOk,
    kInvalidOid,
    kFileNotFound,
    kFileCreateFailed,
    kServerCallFailed,
};

// lo_export — write the contents of large object `oid` to local file `path`.
// Returns kOk on success.
LargeObjectResult lo_export(PsqlClient& client, int64_t oid, const std::string& path);

// lo_import — read local file `path` into a new large object, returning the
// new OID in `out_oid`. `comment` is stored in pg_description (best-effort).
LargeObjectResult lo_import(PsqlClient& client, const std::string& path, const std::string& comment,
                            int64_t& out_oid);

// lo_unlink — delete the large object `oid`.
LargeObjectResult lo_unlink(PsqlClient& client, int64_t oid);

// lo_list — print the list of large objects ( OID | Owner | Comment ) to `out`.
// Returns kOk on success (the listing itself is a query result).
LargeObjectResult lo_list(PsqlClient& client, std::ostream& out);

}  // namespace pgcpp::tools
