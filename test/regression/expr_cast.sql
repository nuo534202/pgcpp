-- expr_cast.sql — Type cast operations and conversions.
--
-- Tests explicit casts (::), CAST() syntax, and implicit conversions.

-- Numeric casts
SELECT 1::INTEGER AS int;
SELECT 1::BIGINT AS bigint;
SELECT 1::REAL AS real;
SELECT 1::DOUBLE PRECISION AS dbl;
SELECT 1::NUMERIC AS numeric;
SELECT 1.5::INTEGER AS trunc_cast;
SELECT 1.9::INTEGER AS trunc_up;

-- Text casts
SELECT 42::TEXT AS int_to_text;
SELECT 3.14::TEXT AS num_to_text;
SELECT 'hello'::VARCHAR(20) AS text_to_varchar;
SELECT 'hello'::CHAR(10) AS text_to_char;

-- CAST() function form
SELECT CAST(1 AS INTEGER) AS cast_int;
SELECT CAST('42' AS INTEGER) AS cast_str_int;
SELECT CAST('2023-01-15' AS DATE) AS cast_str_date;
SELECT CAST(3.14 AS INTEGER) AS cast_trunc;

-- Bool casts
SELECT 1::BOOLEAN AS int_to_bool;
SELECT 0::BOOLEAN AS zero_to_bool;
SELECT 'true'::BOOLEAN AS str_to_bool;

-- Date/time casts
SELECT '2023-01-15'::DATE AS str_to_date;
SELECT '2023-01-15 10:30:00'::TIMESTAMP AS str_to_ts;
SELECT '2023-01-15'::TIMESTAMP AS date_to_ts;
SELECT '2023-01-15 10:30:00'::TIMESTAMP::DATE AS ts_to_date;

-- Round-trip casts
SELECT (1::TEXT)::INTEGER AS int_roundtrip;
SELECT ('2023-01-15'::DATE)::TEXT AS date_roundtrip;

-- Invalid casts (should error)
-- SELECT 'abc'::INTEGER AS invalid;
-- SELECT '2023-13-45'::DATE AS invalid_date;

-- Cast in expression
SELECT (1 + 2)::TEXT || ' is three' AS concat_cast;
SELECT 1::REAL + 2::INTEGER AS mixed_cast;
