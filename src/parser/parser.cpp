// parser.cpp — Raw parser entry point for MyToyDB.
//
// Converted from PostgreSQL 15's src/backend/parser/parser.c.
// Provides raw_parser(), the public entry point that drives the Bison
// C++ parser. Also implements BisonParser::error() since the grammar
// file (gram.yy) has an empty epilogue.

#include "pgcpp/parser/parser.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "gram.tab.hpp"
#include "pgcpp/common/error/elog.hpp"
#include "pgcpp/parser/parser_driver.hpp"

// BisonParser::error — called by the Bison parser on syntax errors.
// We must NOT ereport(ERROR) here because the longjmp would skip the
// destructor of the BisonParser object (a local variable in raw_parser),
// leaking its internal std::vector stacks. Instead we report at WARNING
// level and let raw_parser escalate to ERROR after the parser object is
// safely destructed.
void mytoydb_parser::BisonParser::error(const location_type& loc, const std::string& msg) {
    std::fprintf(stderr, "syntax error at location %d: %s\n", loc, msg.c_str());
}

namespace mytoydb::parser {

// raw_parser — parse a SQL string into a list of RawStmt nodes.
// Returns a list of RawStmt* (one per statement in the input).
// On error, the Bison parser returns non-zero; we then ereport(ERROR).
std::vector<RawStmt*> raw_parser(const std::string& str) {
    std::vector<RawStmt*> parsetree;
    int result = 0;
    {
        ParserDriver driver;
        driver.scanbuf = str;
        driver.scanpos = 0;

        mytoydb_parser::BisonParser parser(driver);
        if (::getenv("YYDEBUG"))
            parser.set_debug_level(1);
        result = parser.parse();
        parsetree = std::move(driver.parsetree);
    }  // parser and driver destructed here — before any ereport

    if (result != 0) {
        // The parser's error handler already printed the message to stderr.
        // Now that the parser object is destructed, escalate to ERROR.
        ereport(mytoydb::error::LogLevel::kError, "syntax error in input SQL");
    }

    return parsetree;
}

}  // namespace mytoydb::parser
