#pragma once

#include <string>
#include <string_view>

#include "mytoydb/types/datum.hpp"

namespace mytoydb::types {

// length(text) — return the number of characters in a text value.
// Returns an int4 Datum.
Datum text_length(Datum value);

// like(text, pattern) — SQL LIKE matching.
// % matches any sequence of characters, _ matches any single character.
// Returns a bool Datum (true if the text matches the pattern).
Datum like(Datum text, Datum pattern);

// not_like(text, pattern) — SQL NOT LIKE.
// Returns a bool Datum (true if the text does NOT match the pattern).
Datum not_like(Datum text, Datum pattern);

// regexp_replace(source, pattern, replacement) — replace text matching a
// POSIX regular expression. If the pattern has no capture groups, the entire
// match is replaced. If it has capture groups, \1, \2, etc. in the replacement
// string refer to the captured groups.
// Returns a text Datum with the replacement applied.
Datum regexp_replace(Datum source, Datum pattern, Datum replacement);

// substring(text, pattern) — extract substring matching a POSIX regex.
// Returns a text Datum with the first match (or the first capture group if
// the pattern has one).
Datum substring(Datum text, Datum pattern);

}  // namespace mytoydb::types
