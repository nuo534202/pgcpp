-- agg_group_by.sql — GROUP BY, HAVING, and grouped aggregates.
--
-- Tests single-column and multi-column grouping, HAVING filtering,
-- and aggregate expressions in SELECT.

CREATE TABLE sales (
    id      INTEGER,
    region  TEXT,
    product TEXT,
    qty     INTEGER,
    price   INTEGER
);
INSERT INTO sales VALUES (1, 'north', 'A', 10, 100);
INSERT INTO sales VALUES (2, 'north', 'B', 5, 200);
INSERT INTO sales VALUES (3, 'north', 'A', 8, 100);
INSERT INTO sales VALUES (4, 'south', 'A', 20, 100);
INSERT INTO sales VALUES (5, 'south', 'B', 12, 200);
INSERT INTO sales VALUES (6, 'south', 'A', 15, 100);
INSERT INTO sales VALUES (7, 'east',  'B', 7, 200);

-- Single-column GROUP BY
SELECT region, COUNT(*) FROM sales GROUP BY region ORDER BY region;
SELECT region, SUM(qty) FROM sales GROUP BY region ORDER BY region;
SELECT region, AVG(price) FROM sales GROUP BY region ORDER BY region;
SELECT region, MIN(qty), MAX(qty) FROM sales GROUP BY region ORDER BY region;

-- Multi-column GROUP BY
SELECT region, product, COUNT(*) FROM sales GROUP BY region, product ORDER BY region, product;
SELECT region, product, SUM(qty) FROM sales GROUP BY region, product ORDER BY region, product;

-- GROUP BY with HAVING
SELECT region, COUNT(*) FROM sales GROUP BY region HAVING COUNT(*) > 2 ORDER BY region;
SELECT region, SUM(qty) FROM sales GROUP BY region HAVING SUM(qty) > 20 ORDER BY region;

-- GROUP BY with computed aggregate
SELECT region, SUM(qty * price) AS revenue FROM sales GROUP BY region ORDER BY region;
SELECT region, COUNT(DISTINCT product) FROM sales GROUP BY region ORDER BY region;

-- GROUP BY with WHERE (filter before group)
SELECT region, COUNT(*) FROM sales WHERE product = 'A' GROUP BY region ORDER BY region;
SELECT region, SUM(qty) FROM sales WHERE qty > 10 GROUP BY region ORDER BY region;

-- Empty group result
SELECT region, COUNT(*) FROM sales WHERE qty > 1000 GROUP BY region ORDER BY region;

DROP TABLE sales;
