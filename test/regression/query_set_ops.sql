-- query_set_ops.sql — Set operations: UNION, UNION ALL, INTERSECT, EXCEPT.
--
-- Tests set-operation execution, deduplication, and column resolution.

CREATE TABLE set_a (id INTEGER, val TEXT);
CREATE TABLE set_b (id INTEGER, val TEXT);

INSERT INTO set_a VALUES (1, 'a');
INSERT INTO set_a VALUES (2, 'b');
INSERT INTO set_a VALUES (3, 'c');
INSERT INTO set_a VALUES (4, 'a');

INSERT INTO set_b VALUES (1, 'a');
INSERT INTO set_b VALUES (2, 'x');
INSERT INTO set_b VALUES (3, 'c');
INSERT INTO set_b VALUES (5, 'y');

-- UNION ALL (no dedup)
SELECT id, val FROM set_a UNION ALL SELECT id, val FROM set_b ORDER BY id, val;

-- UNION (dedup)
SELECT id, val FROM set_a UNION SELECT id, val FROM set_b ORDER BY id, val;

-- INTERSECT
SELECT id, val FROM set_a INTERSECT SELECT id, val FROM set_b ORDER BY id, val;

-- EXCEPT
SELECT id, val FROM set_a EXCEPT SELECT id, val FROM set_b ORDER BY id, val;

-- INTERSECT ALL / EXCEPT ALL
SELECT val FROM set_a INTERSECT ALL SELECT val FROM set_b ORDER BY val;
SELECT val FROM set_a EXCEPT ALL SELECT val FROM set_b ORDER BY val;

-- Three-way UNION
SELECT id FROM set_a WHERE id < 3
UNION
SELECT id FROM set_b WHERE id < 3
UNION
SELECT 99 AS id
ORDER BY id;

-- UNION with different column types (should auto-cast)
SELECT 1::INTEGER AS num UNION SELECT '2'::TEXT ORDER BY num;

-- UNION with aggregate
SELECT COUNT(*) AS cnt FROM set_a UNION SELECT COUNT(*) FROM set_b ORDER BY cnt;

DROP TABLE set_a;
DROP TABLE set_b;
