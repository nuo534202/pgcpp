#pragma once

#include <string>
#include <string_view>

#include "types/datum.hpp"

namespace pgcpp::types {

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

// regexp_replace(source, pattern, replacement) — replace the FIRST substring
// matching a POSIX regular expression. If the pattern has capture groups,
// \1, \2, etc. in the replacement string refer to the captured groups.
// This matches PostgreSQL's default behavior (no 'g' flag).
// Returns a text Datum with the replacement applied.
Datum regexp_replace(Datum source, Datum pattern, Datum replacement);

// regexp_replace(source, pattern, replacement, flags) — 4-arg form.
// If flags contains 'g', ALL matches are replaced; otherwise only the first.
Datum regexp_replace(Datum source, Datum pattern, Datum replacement, Datum flags);

// substring(text, pattern) — extract substring matching a POSIX regex.
// Returns a text Datum with the first match (or the first capture group if
// the pattern has one).
Datum substring(Datum text, Datum pattern);

// --- varlena text functions (PostgreSQL-style) ---

// textcat(a, b) — text concatenation (alias of text_concat).
Datum textcat(Datum a, Datum b);

// text_substr(str, start, len) — substr(text, from, len).
// `start` and `len` are int4 Datums. PostgreSQL's `from` is 1-based; a
// negative `len` is treated as 0.
Datum text_substr(Datum str, Datum start, Datum len);

// text_left(str, n) — first n characters. Negative n returns all but the
// last |n| characters (PostgreSQL semantics).
Datum text_left(Datum str, Datum n);

// text_right(str, n) — last n characters. Negative n returns all but the
// first |n| characters.
Datum text_right(Datum str, Datum n);

// text_upper(str) — ASCII uppercase.
Datum text_upper(Datum str);

// text_lower(str) — ASCII lowercase.
Datum text_lower(Datum str);

// text_btrim(str) — trim whitespace (space, tab, newline) from both ends.
Datum text_btrim(Datum str);

// text_ltrim(str) — trim leading whitespace.
Datum text_ltrim(Datum str);

// text_rtrim(str) — trim trailing whitespace.
Datum text_rtrim(Datum str);

// text_repeat(str, n) — repeat the string n times.
Datum text_repeat(Datum str, Datum n);

// text_reverse(str) — reverse the string.
Datum text_reverse(Datum str);

// --- Task 10 additions (replace / position / lpad / rpad / split_part /
// substr 2-arg / trim 2-arg variants) ---

// text_replace(source, from, to) — replace every occurrence of `from` in
// `source` with `to`. Empty `from` is a no-op (returns source unchanged).
Datum text_replace(Datum source, Datum from, Datum to);

// text_position(string, substring) — 1-based index of the first occurrence
// of `substring` in `string`, or 0 if not found.
Datum text_position(Datum string, Datum substring);

// text_lpad(string, length) — left-pad `string` with spaces to `length`
// characters. If `string` is already longer, it is truncated to `length`.
Datum text_lpad(Datum string, Datum length);

// text_lpad(string, length, fill) — left-pad `string` with `fill` to
// `length` characters.
Datum text_lpad(Datum string, Datum length, Datum fill);

// text_rpad(string, length) — right-pad `string` with spaces to `length`
// characters.
Datum text_rpad(Datum string, Datum length);

// text_rpad(string, length, fill) — right-pad `string` with `fill` to
// `length` characters.
Datum text_rpad(Datum string, Datum length, Datum fill);

// text_split_part(string, sep, field) — return the `field`-th field
// (1-based) of `string` split on `sep`. Negative `field` counts from the
// end. Out-of-range returns the empty string.
Datum text_split_part(Datum string, Datum sep, Datum field);

// text_substr_2(str, start) — substring starting at 1-based `start` to
// the end of the string. Negative `start` is treated as 1.
Datum text_substr_2(Datum str, Datum start);

// text_trim(str) — alias of text_btrim; trims whitespace from both ends.
Datum text_trim(Datum str);

}  // namespace pgcpp::types
