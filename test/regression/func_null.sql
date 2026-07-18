-- func_null.sql — NULL-handling functions.
--
-- Tests COALESCE, NULLIF, GREATEST, LEAST, IS [NOT] NULL semantics.

-- COALESCE
SELECT COALESCE(NULL, 'default') AS coalesce_default;
SELECT COALESCE(NULL, NULL, 'third') AS coalesce_third;
SELECT COALESCE('first', 'second') AS coalesce_first;
SELECT COALESCE(NULL, NULL, NULL) AS coalesce_all_null;
SELECT COALESCE(NULL, 0) AS coalesce_zero;

-- NULLIF
SELECT NULLIF(1, 1) AS nullif_equal;
SELECT NULLIF(1, 2) AS nullif_diff;
SELECT NULLIF('a', 'a') AS nullif_str;
SELECT NULLIF(NULL, NULL) AS nullif_null;

-- GREATEST / LEAST
SELECT GREATEST(1, 2, 3) AS greatest;
SELECT GREATEST(1, NULL, 3) AS greatest_with_null;
SELECT LEAST(1, 2, 3) AS least;
SELECT LEAST(1, NULL, 3) AS least_with_null;
SELECT GREATEST('a', 'b', 'c') AS greatest_str;

-- NULL arithmetic
SELECT NULL + 1 AS null_plus;
SELECT NULL - 1 AS null_minus;
SELECT NULL * 0 AS null_times_zero;
SELECT NULL || 'x' AS null_concat;

-- NULL comparison
SELECT NULL = NULL AS null_eq;
SELECT NULL <> NULL AS null_ne;
SELECT NULL = 1 AS null_eq_int;
SELECT NULL < 1 AS null_lt;

-- NULL in aggregates
CREATE TABLE null_agg (id INTEGER, val INTEGER);
INSERT INTO null_agg VALUES (1, 10);
INSERT INTO null_agg VALUES (2, NULL);
INSERT INTO null_agg VALUES (3, 30);
INSERT INTO null_agg VALUES (4, NULL);
SELECT COUNT(*) FROM null_agg;
SELECT COUNT(val) FROM null_agg;
SELECT SUM(val) FROM null_agg;
SELECT AVG(val) FROM null_agg;
SELECT MIN(val), MAX(val) FROM null_agg;
SELECT SUM(val) / COUNT(val) AS manual_avg FROM null_agg;
DROP TABLE null_agg;
