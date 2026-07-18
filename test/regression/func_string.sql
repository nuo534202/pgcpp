-- func_string.sql — Built-in string functions.
--
-- Tests LENGTH, UPPER, LOWER, SUBSTRING, TRIM, REPLACE, POSITION, etc.

-- Length
SELECT LENGTH('hello') AS len_5;
SELECT LENGTH('') AS len_0;
SELECT CHAR_LENGTH('hello') AS char_len;
SELECT OCTET_LENGTH('hello') AS octet_len;
SELECT BIT_LENGTH('hello') AS bit_len;

-- Case conversion
SELECT UPPER('hello') AS upper;
SELECT LOWER('HELLO') AS lower;
SELECT INITCAP('hello world') AS initcap;

-- Substring
SELECT SUBSTRING('hello' FROM 2 FOR 3) AS substr_from_for;
SELECT SUBSTRING('hello' FROM 2) AS substr_from;
SELECT SUBSTR('hello', 2, 3) AS substr_args;
SELECT LEFT('hello', 3) AS left_3;
SELECT RIGHT('hello', 3) AS right_3;

-- Trim
SELECT TRIM('  hello  ') AS trim_both;
SELECT LTRIM('  hello') AS ltrim;
SELECT RTRIM('hello  ') AS rtrim;
SELECT BTRIM('xxhelloxx', 'x') AS btrim_chars;

-- Replace / Position
SELECT REPLACE('hello', 'l', 'L') AS replace;
SELECT POSITION('l' IN 'hello') AS position;
SELECT STRPOS('hello', 'l') AS strpos;

-- Padding
SELECT LPAD('5', 3, '0') AS lpad;
SELECT RPAD('5', 3, '0') AS rpad;

-- Repeat / Reverse
SELECT REPEAT('ab', 3) AS repeat;
SELECT REVERSE('hello') AS reverse;

-- Split
SELECT SPLIT_PART('a,b,c', ',', 2) AS split;
