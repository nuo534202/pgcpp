-- agg_basic.sql — Aggregate functions: COUNT, SUM, AVG, MIN, MAX.
--
-- Tests single-row, multi-row, empty-table, and NULL-skipping behavior.

CREATE TABLE agg_t (id INTEGER, val INTEGER, w REAL);
INSERT INTO agg_t VALUES (1, 10, 1.5);
INSERT INTO agg_t VALUES (2, 20, 2.5);
INSERT INTO agg_t VALUES (3, 30, 3.5);
INSERT INTO agg_t VALUES (4, 40, 4.5);
INSERT INTO agg_t VALUES (5, NULL, NULL);

-- COUNT variants
SELECT COUNT(*) FROM agg_t;
SELECT COUNT(val) FROM agg_t;
SELECT COUNT(w) FROM agg_t;

-- SUM / AVG
SELECT SUM(val) FROM agg_t;
SELECT AVG(val) FROM agg_t;
SELECT SUM(w) FROM agg_t;
SELECT AVG(w) FROM agg_t;

-- MIN / MAX
SELECT MIN(val), MAX(val) FROM agg_t;
SELECT MIN(w), MAX(w) FROM agg_t;
SELECT MIN(id), MAX(id) FROM agg_t;

-- Aggregates with WHERE
SELECT COUNT(*) FROM agg_t WHERE val > 20;
SELECT SUM(val) FROM agg_t WHERE val > 20;
SELECT AVG(val) FROM agg_t WHERE id < 4;

-- Empty aggregate (no matching rows)
SELECT COUNT(*) FROM agg_t WHERE val > 1000;
SELECT SUM(val) FROM agg_t WHERE val > 1000;
SELECT AVG(val) FROM agg_t WHERE val > 1000;
SELECT MIN(val) FROM agg_t WHERE val > 1000;
SELECT MAX(val) FROM agg_t WHERE val > 1000;

-- COUNT(DISTINCT ...)
SELECT COUNT(DISTINCT val) FROM agg_t;
SELECT COUNT(DISTINCT w) FROM agg_t;

-- SUM with expression
SELECT SUM(val + 1) FROM agg_t WHERE val IS NOT NULL;
SELECT SUM(val * 2) FROM agg_t WHERE val IS NOT NULL;

DROP TABLE agg_t;
