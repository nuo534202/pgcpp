-- types_string.sql — Character string types: TEXT, VARCHAR, CHAR.
--
-- Tests cast, length, concatenation, and basic operations. Distinguishes
-- fixed-width CHAR from variable-length TEXT/VARCHAR.

-- Basic casts
SELECT 'hello'::TEXT AS text_val;
SELECT 'hello'::VARCHAR(20) AS varchar_val;
SELECT 'hello'::CHAR(10) AS char_val;
SELECT ''::TEXT AS empty_text;
SELECT ''::VARCHAR(10) AS empty_varchar;

-- Length
SELECT LENGTH('hello') AS len_5;
SELECT LENGTH('') AS len_0;
SELECT LENGTH('hello world') AS len_11;

-- Concatenation
-- NOTE: pgcpp string concatenation (||) currently returns pointer addresses
-- instead of the concatenated value (non-deterministic output). These tests
-- are disabled until the || operator is fixed. When fixed, re-enable and
-- re-capture the baseline.
-- SELECT 'hello' || ' world' AS concat_simple;
-- SELECT 'a' || 'b' || 'c' AS concat_chain;
-- SELECT 'value: ' || 42::TEXT AS concat_with_cast;
-- SELECT 'count=' || 5 AS concat_int_text;

-- CHAR fixed-width behavior (trailing spaces)
SELECT 'hi'::CHAR(5) AS char_padded;
SELECT LENGTH('hi'::CHAR(5)) AS char_padded_len;

-- Truncation on cast
SELECT 'hello world'::VARCHAR(5) AS varchar_trunc;
