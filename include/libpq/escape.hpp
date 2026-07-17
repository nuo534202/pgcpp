// escape.hpp — String / identifier / bytea escape utilities (P3-11).
//
// Mirrors PostgreSQL libpq's escape functions:
//   - PQescapeLiteral:   produces 'literal' suitable for SQL string position
//   - PQescapeIdentifier: produces "identifier" suitable for object names
//   - PQescapeString:    raw byte-level escape (no surrounding quotes)
//   - PQescapeByteaConn: escape binary data as \\x.. or octal bytea literal
//   - PQunescapeBytea:   reverse PQescapeByteaConn
//
// These functions are essential for safely constructing SQL strings on
// the client side. They are also used by pg_dump to emit DDL/DML.
//
// The connection-scoped variants (EscapeLiteralConn, etc.) take a PgConn*
// but pgcpp's implementations currently ignore it because the escaping
// rules depend only on the protocol version (3) and standard_conforming_strings
// (true by default in pgcpp). The standalone variants assume the same.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pgcpp::libpq {

class PgConn;

// EscapeStringResult — outcome of EscapeString / EscapeStringConn.
struct EscapeStringResult {
    bool ok = false;            // false on encoding error (non-ASCII in input)
    std::string out;            // escaped string (no surrounding quotes)
    std::string error_message;  // populated when ok == false
};

// EscapeString — escape a string for use in a SQL statement.
//
// Standalone variant (no connection needed). Implements the same logic
// as libpq's PQescapeString with standard_conforming_strings=on: wraps
// in single quotes and escapes special chars (single-quote, backslash).
// Returns the escaped string (with surrounding quotes) via result.
EscapeStringResult EscapeString(const std::string& str);

// EscapeStringConn — connection-scoped variant. Currently identical to
// EscapeString (standard_conforming_strings is on by default).
EscapeStringResult EscapeStringConn(const PgConn& conn, const std::string& str);

// EscapeLiteral — escape a string as a SQL literal (with quotes).
// Mirrors libpq's PQescapeLiteral.
EscapeStringResult EscapeLiteral(const std::string& str);

// EscapeLiteralConn — connection-scoped variant.
EscapeStringResult EscapeLiteralConn(const PgConn& conn, const std::string& str);

// EscapeIdentifier — escape a string as a SQL identifier (double-quoted).
// Validates that the input is a valid identifier (starts with letter or
// underscore, contains only alphanumerics/underscore/$, length <= 63).
// Returns the quoted identifier on success, or ok=false on validation
// error (empty string, invalid chars, too long).
EscapeStringResult EscapeIdentifier(const std::string& str);

// EscapeIdentifierConn — connection-scoped variant.
EscapeStringResult EscapeIdentifierConn(const PgConn& conn, const std::string& str);

// QuoteLiteral — wrap a string in single quotes, doubling embedded
// single-quotes (no backslash escapes). Used for emitting SQL literals
// in pg_dump output where standard_conforming_strings is assumed.
std::string QuoteLiteral(const std::string& str);

// QuoteIdentifier — wrap a string in double quotes, doubling embedded
// double-quotes.
std::string QuoteIdentifier(const std::string& str);

// EscapeBytea — escape a binary buffer as a PostgreSQL bytea literal
// using the modern hex format "\\x...". Mirrors PQescapeByteaConn with
// the hex format. Returns the escaped string (no surrounding quotes).
std::string EscapeBytea(const unsigned char* data, std::size_t len);

// EscapeByteaConn — connection-scoped variant. Identical to EscapeBytea
// in pgcpp (hex format always supported).
std::string EscapeByteaConn(const PgConn& conn, const unsigned char* data, std::size_t len);

// UnescapeByteaResult — outcome of UnescapeBytea.
struct UnescapeByteaResult {
    bool ok = false;                 // false on parse error
    std::vector<unsigned char> out;  // decoded bytes
    std::string error_message;       // populated when ok == false
};

// UnescapeBytea — decode a bytea literal (hex or octal escape form).
// Accepts both "\\x.." (hex) and "\\nnn" (octal) styles produced by
// server-side byteaout. Mirrors PQunescapeBytea.
UnescapeByteaResult UnescapeBytea(const char* str, std::size_t len);

}  // namespace pgcpp::libpq
