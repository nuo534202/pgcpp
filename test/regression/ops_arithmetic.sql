-- ops_arithmetic.sql — Arithmetic operators on numeric types.
-- Tests +, -, *, /, %, and unary minus for correctness and overflow handling.

-- Basic integer arithmetic
SELECT 1 + 1 AS plus;
SELECT 10 - 3 AS minus;
SELECT 4 * 5 AS times;
SELECT 20 / 4 AS int_div;
SELECT 7 / 2 AS int_div_trunc;
SELECT 7 % 3 AS mod;
SELECT -5 AS unary_minus;
SELECT -(5 + 3) AS paren_neg;

-- Operator precedence
SELECT 1 + 2 * 3 AS precedence;
SELECT (1 + 2) * 3 AS paren;
SELECT 10 - 2 - 3 AS left_assoc;
SELECT 2 * 3 + 4 * 5 AS mixed;

-- Floating-point
SELECT 1.5 + 2.5 AS fp_plus;
SELECT 1.5 - 0.5 AS fp_minus;
SELECT 1.5 * 2.0 AS fp_times;
SELECT 7.0 / 2.0 AS fp_div;

-- Numeric
SELECT 1.5::NUMERIC + 2.5::NUMERIC AS num_plus;
SELECT 10::NUMERIC / 3::NUMERIC AS num_div;
SELECT 1.5::NUMERIC * 100::NUMERIC AS num_times;

-- Division by zero (should error)
-- SELECT 1 / 0 AS div_zero;
-- SELECT 1.0 / 0.0 AS fp_div_zero;

-- Overflow (may error or wrap, depending on type)
-- SELECT 9223372036854775807::BIGINT + 1 AS bigint_overflow;
