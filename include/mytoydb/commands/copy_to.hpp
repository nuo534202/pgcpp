// copy_to.h — COPY TO implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/copyto.c.
// Scans a relation and writes rows as text to a file.
#pragma once

#include <cstdint>
#include <string>

namespace mytoydb::access {
struct RelationData;
}  // namespace mytoydb::access

namespace mytoydb::commands {

// CopyToText — scan `rel` and write tab-delimited rows to `filename`.
// Returns the number of rows written.
int64_t CopyToText(access::RelationData* rel, const std::string& filename);

}  // namespace mytoydb::commands
