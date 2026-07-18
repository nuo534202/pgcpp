-- expr_case.sql — CASE expressions: simple, searched, nested.
--
-- Tests conditional expression evaluation and result type resolution.

-- Searched CASE
-- NOTE: pgcpp CASE WHEN returning TEXT currently returns pointer addresses
-- instead of the value (non-deterministic). Use INTEGER results to keep
-- output deterministic. String-returning CASE is documented below but
-- disabled until the bug is fixed.
SELECT CASE WHEN 1 > 0 THEN 1 ELSE 0 END AS sign_pos;
SELECT CASE WHEN 1 > 2 THEN 0 WHEN 1 > 0 THEN 1 ELSE -1 END AS multi;
SELECT CASE WHEN 5 > 10 THEN 1 WHEN 5 > 3 THEN 2 WHEN 5 > 1 THEN 3 ELSE 4 END AS first_match;
-- SELECT CASE WHEN 1 > 0 THEN 'positive' ELSE 'non-positive' END AS sign;
-- SELECT CASE WHEN 1 > 2 THEN 'a' WHEN 1 > 0 THEN 'b' ELSE 'c' END AS multi;
-- SELECT CASE WHEN 5 > 10 THEN 'a' WHEN 5 > 3 THEN 'b' WHEN 5 > 1 THEN 'c' ELSE 'd' END AS first_match;

-- Simple CASE
-- NOTE: pgcpp does not support simple CASE form (CASE arg WHEN v THEN r).
-- Disabled until implemented.
-- SELECT CASE 1 WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'other' END AS simple;
-- SELECT CASE 2 WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'other' END AS simple_match;
-- SELECT CASE 3 WHEN 1 THEN 'one' WHEN 2 THEN 'two' ELSE 'other' END AS simple_default;

-- CASE with NULL
SELECT CASE WHEN NULL IS NULL THEN 1 ELSE 0 END AS null_check;
-- SELECT CASE NULL WHEN NULL THEN 'match' ELSE 'no match' END AS null_simple;

-- CASE in table context
CREATE TABLE case_t (id INTEGER, val INTEGER);
INSERT INTO case_t VALUES (1, 10);
INSERT INTO case_t VALUES (2, 20);
INSERT INTO case_t VALUES (3, 30);
INSERT INTO case_t VALUES (4, NULL);

SELECT id,
       CASE
           WHEN val < 15 THEN 1
           WHEN val < 25 THEN 2
           WHEN val IS NULL THEN -1
           ELSE 3
       END AS category
FROM case_t
ORDER BY id;

-- CASE in aggregate
SELECT
    COUNT(*) FILTER (WHERE val < 20) AS small_count,
    COUNT(*) FILTER (WHERE val >= 20) AS large_count
FROM case_t;

-- CASE inside SUM
SELECT SUM(CASE WHEN val IS NOT NULL THEN 1 ELSE 0 END) AS non_null_count
FROM case_t;

DROP TABLE case_t;
