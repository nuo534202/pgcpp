-- basic_select.sql — Exercise SELECT literal and arithmetic expressions.
-- Verifies the simplest read path: no tables, no DML, just expression
-- evaluation and result-set formatting.

SELECT 1;
SELECT 1 + 2 AS sum;
SELECT 10 - 3 AS diff;
SELECT 4 * 5 AS product;
SELECT 100 / 4 AS quotient;
SELECT 1, 2, 3;
SELECT 1 AS a, 42 AS answer;
