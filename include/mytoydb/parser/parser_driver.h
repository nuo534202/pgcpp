// parser_driver.h — Driver class connecting the scanner and Bison parser.
//
// Converted from PostgreSQL 15's parser/parser.h and gramparse.h.
// The ParserDriver holds the scanner state, the result parsetree, and
// provides the interface between the Flex scanner and the Bison parser.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "mytoydb/common/containers/node.h"
#include "mytoydb/parser/parsenodes.h"

namespace mytoydb::parser {

// ParserDriver — glue object passed to the Bison parser via %param.
// It holds:
//   * the input string (scanbuf),
//   * the current scan location,
//   * the resulting list of RawStmt nodes (one per top-level statement).
//
// The scanner (not yet implemented) will read from scanbuf and update
// `location`. The parser appends finished RawStmt nodes to `parsetree`.
class ParserDriver {
public:
    // The resulting parse tree: one RawStmt per top-level statement.
    std::vector<RawStmt*> parsetree;

    // The input SQL string being parsed.
    std::string scanbuf;

    // Current byte offset into scanbuf (updated by the scanner).
    int location = 0;

    // Current scan position in scanbuf (advanced by the hand-written scanner).
    size_t scanpos = 0;
};

}  // namespace mytoydb::parser
