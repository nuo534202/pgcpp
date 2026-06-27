// copy_from.h — COPY FROM implementation.
//
// Converted from PostgreSQL 15's src/backend/commands/copyfrom.c.
// Reads text rows from a file and inserts them into a relation.
#pragma once

#include <cstdint>
#include <string>

namespace mytoydb::access {
struct RelationData;
}  // namespace mytoydb::access

namespace mytoydb::commands {

// CopyFromText — read tab-delimited text rows from `filename` and insert
// into the opened relation `rel`. Returns the number of rows inserted.
int64_t CopyFromText(access::RelationData* rel, const std::string& filename);

}  // namespace mytoydb::commands
