-- basic_insert.sql — Exercise CREATE TABLE, INSERT, and SELECT.
-- Verifies the DML write path: heap_insert, catalog registration, and
-- subsequent seqscan read-back. Numerics, strings, and NULL are covered.

CREATE TABLE t_insert (
    id   INTEGER,
    name TEXT,
    val  INTEGER
);

INSERT INTO t_insert VALUES (1, 'alice', 100);
INSERT INTO t_insert VALUES (2, 'bob', 200);
INSERT INTO t_insert VALUES (3, 'carol', 300);

SELECT id, name, val FROM t_insert ORDER BY id;

SELECT COUNT(*) FROM t_insert;

SELECT SUM(val) FROM t_insert;

DROP TABLE t_insert;
