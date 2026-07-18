-- func_math.sql — Built-in mathematical functions.
--
-- Tests ABS, ROUND, FLOOR, CEIL, POWER, SQRT, MOD, etc.

SELECT ABS(-5) AS abs_neg;
SELECT ABS(5) AS abs_pos;
SELECT ABS(-3.14) AS abs_real;

SELECT ROUND(1.5) AS round_half_up;
SELECT ROUND(1.4) AS round_down;
SELECT ROUND(2.5) AS round_half_even;
SELECT ROUND(1.567, 2) AS round_scale;
SELECT ROUND(-1.5) AS round_neg;

SELECT FLOOR(1.7) AS floor;
SELECT FLOOR(-1.2) AS floor_neg;
SELECT CEIL(1.2) AS ceil;
SELECT CEIL(-1.7) AS ceil_neg;
SELECT CEILING(1.2) AS ceiling_alias;

SELECT POWER(2, 3) AS power;
SELECT POWER(2.0, 0.5) AS sqrt_via_power;
SELECT SQRT(16) AS sqrt;
SELECT SQRT(2) AS sqrt_2;

SELECT MOD(7, 3) AS mod;
SELECT 7 % 3 AS mod_op;

SELECT EXP(1) AS exp;
SELECT LN(2.718281828) AS ln;
SELECT LOG(10) AS log;
SELECT LOG(2, 8) AS log_base;

SELECT SIGN(-5) AS sign_neg;
SELECT SIGN(0) AS sign_zero;
SELECT SIGN(5) AS sign_pos;

SELECT PI() AS pi;
SELECT DEGREES(3.141592653589793) AS degrees;
SELECT RADIANS(180) AS radians;

SELECT TRUNC(1.9) AS trunc;
SELECT TRUNC(1.567, 1) AS trunc_scale;
