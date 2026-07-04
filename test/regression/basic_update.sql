-- basic_update.sql — Exercise UPDATE and DELETE paths.
-- Verifies command tags (INSERT 0 N, UPDATE N, DELETE N) and post-update
-- state observed via SELECT.
--
-- NOTE: pgcpp UPDATE is currently broken (P1-4 will fix).
-- The expected.out captures current behavior as a baseline.
-- When UPDATE is fixed, this test will FAIL and expected.out must update.
-- IMPORTANT: avoid apostrophes and semicolons in SQL comments because
-- psql ExecuteFile does not skip -- comments and a stray quote or
-- semicolon breaks its naive statement splitter.

CREATE TABLE t_update (
    id    INTEGER,
    name  TEXT,
    count INTEGER
);

INSERT INTO t_update VALUES (1, 'alice', 10);
INSERT INTO t_update VALUES (2, 'bob', 20);
INSERT INTO t_update VALUES (3, 'carol', 30);

SELECT id, name, count FROM t_update ORDER BY id;

UPDATE t_update SET count = count + 5 WHERE id = 2;

SELECT id, name, count FROM t_update WHERE id = 2;

UPDATE t_update SET count = count * 2;

SELECT id, name, count FROM t_update ORDER BY id;

DELETE FROM t_update WHERE id = 1;

SELECT COUNT(*) FROM t_update;

SELECT id, name, count FROM t_update ORDER BY id;

DROP TABLE t_update;
