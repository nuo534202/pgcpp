-- types_numeric.sql — Numeric data types: INTEGER, BIGINT, SMALLINT,
-- REAL, DOUBLE PRECISION, NUMERIC.
--
-- Tests cast, storage, retrieval, and basic arithmetic for each numeric
-- type. Exposes type-system gaps in a rewritten engine.

-- Integer family
SELECT 1::INTEGER AS int_pos;
SELECT -1::INTEGER AS int_neg;
SELECT 0::INTEGER AS int_zero;
SELECT 2147483647::INTEGER AS int_max;
SELECT -2147483648::INTEGER AS int_min;

SELECT 1::BIGINT AS bigint_pos;
SELECT 9223372036854775807::BIGINT AS bigint_max;
SELECT -9223372036854775808::BIGINT AS bigint_min;

SELECT 1::SMALLINT AS smallint_pos;
SELECT 32767::SMALLINT AS smallint_max;
SELECT -32768::SMALLINT AS smallint_min;

-- Floating-point family
SELECT 3.14::REAL AS real_pi;
SELECT -3.14::REAL AS real_neg_pi;
SELECT 0.0::REAL AS real_zero;

SELECT 3.141592653589793::DOUBLE PRECISION AS dbl_pi;
SELECT -3.141592653589793::DOUBLE PRECISION AS dbl_neg_pi;
SELECT 0.0::DOUBLE PRECISION AS dbl_zero;

-- NUMERIC with precision/scale
SELECT 1.5::NUMERIC AS num_plain;
SELECT 1.5::NUMERIC(10,2) AS num_scale2;
SELECT 1.5::NUMERIC(20,5) AS num_scale5;
SELECT -123.456::NUMERIC(10,3) AS num_neg;

-- Mixed-type arithmetic
SELECT 1::INTEGER + 2::INTEGER AS int_plus;
SELECT 1::INTEGER + 2::BIGINT AS int_plus_big;
SELECT 1::INTEGER + 2::REAL AS int_plus_real;
SELECT 1::REAL + 2::DOUBLE PRECISION AS real_plus_dbl;
SELECT 1::INTEGER + 2::NUMERIC AS int_plus_num;

-- Division behavior
SELECT 7::INTEGER / 2::INTEGER AS int_div;
SELECT 7.0::NUMERIC / 2::NUMERIC AS num_div;
SELECT 7::REAL / 2::REAL AS real_div;
