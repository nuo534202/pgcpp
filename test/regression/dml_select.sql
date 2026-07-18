-- dml_select.sql — SELECT variations: WHERE, ORDER BY, LIMIT, OFFSET, DISTINCT.
--
-- Tests query path: scan, filter, sort, limit, deduplication.

CREATE TABLE dml_t (id INTEGER, name TEXT, val INTEGER);
INSERT INTO dml_t VALUES (1, 'alice', 100);
INSERT INTO dml_t VALUES (2, 'bob', 200);
INSERT INTO dml_t VALUES (3, 'carol', 300);
INSERT INTO dml_t VALUES (4, 'dave', 200);
INSERT INTO dml_t VALUES (5, 'eve', 100);

-- Basic select
SELECT * FROM dml_t ORDER BY id;
SELECT id, name FROM dml_t ORDER BY id;
SELECT id FROM dml_t ORDER BY id;

-- WHERE
SELECT * FROM dml_t WHERE id = 3;
SELECT * FROM dml_t WHERE id > 2;
SELECT * FROM dml_t WHERE val = 200;
SELECT * FROM dml_t WHERE name = 'alice';
SELECT * FROM dml_t WHERE id <> 3;
SELECT * FROM dml_t WHERE id >= 2 AND id <= 4;
SELECT * FROM dml_t WHERE id < 2 OR id > 4;
SELECT * FROM dml_t WHERE id IN (1, 3, 5);
SELECT * FROM dml_t WHERE id NOT IN (1, 3, 5);
SELECT * FROM dml_t WHERE name LIKE 'a%';
SELECT * FROM dml_t WHERE val BETWEEN 150 AND 250;

-- ORDER BY
SELECT * FROM dml_t ORDER BY val;
SELECT * FROM dml_t ORDER BY val DESC;
SELECT * FROM dml_t ORDER BY val, id;
SELECT * FROM dml_t ORDER BY val DESC, id ASC;
SELECT * FROM dml_t ORDER BY name;

-- LIMIT / OFFSET
SELECT * FROM dml_t ORDER BY id LIMIT 3;
SELECT * FROM dml_t ORDER BY id LIMIT 2 OFFSET 1;
SELECT * FROM dml_t ORDER BY id LIMIT 100;
SELECT * FROM dml_t ORDER BY id OFFSET 3;

-- DISTINCT
SELECT DISTINCT val FROM dml_t ORDER BY val;
SELECT DISTINCT name FROM dml_t ORDER BY name;
SELECT DISTINCT val, name FROM dml_t ORDER BY val, name;

-- Combinations
SELECT DISTINCT val FROM dml_t WHERE id > 1 ORDER BY val;
SELECT * FROM dml_t WHERE val = 200 ORDER BY id LIMIT 1;

DROP TABLE dml_t;
