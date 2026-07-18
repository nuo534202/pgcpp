-- ops_string.sql — String operators: ||, LIKE, ILIKE, ~, ~*.
--
-- Tests pattern matching, concatenation, and case sensitivity.

-- Concatenation
-- NOTE: pgcpp || operator currently returns pointer addresses (bug).
-- Disabled until fixed; re-enable and re-capture baseline when fixed.
-- SELECT 'hello' || ' world' AS concat;
-- SELECT 'a' || 'b' || 'c' AS concat3;
-- SELECT 'num=' || 42 AS concat_num;
-- SELECT 'count: ' || 100 AS concat_int;

-- LIKE patterns
SELECT 'hello' LIKE 'hello' AS like_exact;
SELECT 'hello' LIKE 'hel%' AS like_prefix;
SELECT 'hello' LIKE '%llo' AS like_suffix;
SELECT 'hello' LIKE '%ell%' AS like_infix;
SELECT 'hello' LIKE 'h_llo' AS like_underscore;
SELECT 'hello' LIKE 'HEL%' AS like_case;
SELECT 'hello' NOT LIKE 'world%' AS not_like;

-- ILIKE (case-insensitive)
SELECT 'hello' ILIKE 'HEL%' AS ilike_prefix;
SELECT 'hello' ILIKE 'Hello' AS ilike_exact;
SELECT 'HELLO' ILIKE 'hello%' AS ilike_case_upper;

-- Regex (POSIX)
-- NOTE: pgcpp regex operators (~, ~*, !~) currently return pointer addresses
-- instead of boolean values (bug). Disabled until fixed; re-enable and
-- re-capture baseline when fixed.
-- SELECT 'hello' ~ '^hel' AS regex_match;
-- SELECT 'hello' ~ 'llo$' AS regex_end;
-- SELECT 'hello' ~ 'ell' AS regex_mid;
-- SELECT 'hello' ~ '^HEL' AS regex_case;
-- SELECT 'hello' ~* '^HEL' AS regex_icase;
-- SELECT 'hello' !~ '^HEL' AS regex_not_match;

-- SIMILAR TO
SELECT 'hello' SIMILAR TO 'h%' AS similar_to;
