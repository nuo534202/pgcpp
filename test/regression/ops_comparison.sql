-- ops_comparison.sql — Comparison operators and predicates.
-- Tests =, <>, <, >, <=, >=, BETWEEN, IN, IS [NOT] NULL.

-- Basic comparison
SELECT 1 = 1 AS eq;
SELECT 1 = 2 AS neq;
SELECT 1 <> 2 AS ne;
SELECT 1 < 2 AS lt;
SELECT 2 > 1 AS gt;
SELECT 1 <= 1 AS le;
SELECT 1 >= 1 AS ge;

-- String comparison
SELECT 'abc' = 'abc' AS str_eq;
SELECT 'abc' <> 'abd' AS str_ne;
SELECT 'abc' < 'abd' AS str_lt;
SELECT 'abc' > 'aba' AS str_gt;

-- Mixed-type comparison
SELECT 1 = 1.0 AS int_eq_real;
SELECT 1 < 1.5 AS int_lt_real;

-- BETWEEN
SELECT 5 BETWEEN 1 AND 10 AS in_range;
SELECT 5 BETWEEN 6 AND 10 AS out_range;
SELECT 5 NOT BETWEEN 6 AND 10 AS not_between;

-- IN
SELECT 1 IN (1, 2, 3) AS in_list;
SELECT 5 IN (1, 2, 3) AS not_in_list;
SELECT 'a' IN ('a', 'b', 'c') AS str_in;
SELECT NULL IN (1, 2, 3) AS null_in;

-- IS NULL / IS NOT NULL
SELECT NULL IS NULL AS null_is_null;
SELECT 1 IS NULL AS one_is_null;
SELECT NULL IS NOT NULL AS null_not_null;
SELECT 1 IS NOT NULL AS one_not_null;

-- IS TRUE / IS FALSE / IS UNKNOWN
SELECT TRUE IS TRUE AS t_is_t;
SELECT FALSE IS TRUE AS f_is_t;
SELECT NULL::BOOLEAN IS TRUE AS null_is_t;
SELECT NULL::BOOLEAN IS UNKNOWN AS null_is_unknown;
