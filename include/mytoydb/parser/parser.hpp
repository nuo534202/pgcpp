#pragma once

#include <vector>

#include "mytoydb/common/containers/node.hpp"
#include "mytoydb/parser/parsenodes.hpp"

namespace mytoydb::parser {

// Public parser API — equivalent to PostgreSQL's parser/parser.h

// raw_parser — parse a SQL string into a list of RawStmt nodes.
// Returns a list of RawStmt* (one per statement in the input).
// On error, calls ereport(ERROR) which longjmps out.
std::vector<RawStmt*> raw_parser(const std::string& str);

}  // namespace mytoydb::parser
