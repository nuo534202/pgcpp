-- query_cte_window.sql — CTE (WITH) and window functions.
--
-- Tests WITH clause (common table expressions) and window function
-- evaluation (ROW_NUMBER, RANK, SUM OVER, etc.).

-- Simple CTE
WITH cte AS (
    SELECT 1 AS x
)
SELECT * FROM cte;

-- Multiple CTEs
WITH cte1 AS (SELECT 1 AS x),
     cte2 AS (SELECT 2 AS y)
SELECT cte1.x, cte2.y FROM cte1, cte2;

-- CTE with table
CREATE TABLE cte_t (id INTEGER, cat TEXT, val INTEGER);
INSERT INTO cte_t VALUES (1, 'a', 10);
INSERT INTO cte_t VALUES (2, 'a', 20);
INSERT INTO cte_t VALUES (3, 'b', 30);
INSERT INTO cte_t VALUES (4, 'b', 40);
INSERT INTO cte_t VALUES (5, 'a', 50);

WITH cat_sum AS (
    SELECT cat, SUM(val) AS total
    FROM cte_t
    GROUP BY cat
)
SELECT * FROM cat_sum ORDER BY cat;

-- CTE referencing CTE
WITH base AS (
    SELECT cat, val FROM cte_t WHERE val > 15
),
agg AS (
    SELECT cat, SUM(val) AS s FROM base GROUP BY cat
)
SELECT * FROM agg ORDER BY cat;

-- Recursive CTE (1 to 10)
WITH RECURSIVE counter(n) AS (
    SELECT 1
    UNION ALL
    SELECT n + 1 FROM counter WHERE n < 10
)
SELECT n FROM counter ORDER BY n;

-- Window functions
SELECT id, cat, val,
       ROW_NUMBER() OVER (PARTITION BY cat ORDER BY val) AS rn
FROM cte_t
ORDER BY cat, val;

SELECT id, cat, val,
       RANK() OVER (PARTITION BY cat ORDER BY val DESC) AS rnk
FROM cte_t
ORDER BY cat, val;

SELECT id, cat, val,
       SUM(val) OVER (PARTITION BY cat) AS cat_total
FROM cte_t
ORDER BY cat, val;

SELECT id, cat, val,
       AVG(val) OVER (PARTITION BY cat) AS cat_avg,
       val - AVG(val) OVER (PARTITION BY cat) AS diff_from_avg
FROM cte_t
ORDER BY cat, val;

SELECT id, cat, val,
       LAG(val, 1) OVER (PARTITION BY cat ORDER BY val) AS prev_val,
       LEAD(val, 1) OVER (PARTITION BY cat ORDER BY val) AS next_val
FROM cte_t
ORDER BY cat, val;

DROP TABLE cte_t;
