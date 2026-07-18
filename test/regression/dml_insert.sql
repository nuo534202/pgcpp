-- dml_insert.sql — INSERT variations: single-row, multi-row, default,
-- column-list, INSERT ... SELECT.
--
-- Tests the write path: heap_insert, catalog registration, default values.

CREATE TABLE ins_t (
    id    INTEGER,
    name  TEXT,
    val   INTEGER DEFAULT 0
);

-- Single-row INSERT
INSERT INTO ins_t VALUES (1, 'a', 10);
INSERT INTO ins_t VALUES (2, 'b', 20);
INSERT INTO ins_t VALUES (3, 'c', 30);
SELECT * FROM ins_t ORDER BY id;

-- INSERT with column list (omit columns)
INSERT INTO ins_t (id, name) VALUES (4, 'd');
SELECT * FROM ins_t WHERE id = 4;

-- INSERT with default
INSERT INTO ins_t VALUES (5, 'e', DEFAULT);
SELECT * FROM ins_t WHERE id = 5;

-- INSERT NULL
INSERT INTO ins_t VALUES (6, NULL, NULL);
SELECT * FROM ins_t WHERE id = 6;

-- Multi-row INSERT
INSERT INTO ins_t VALUES (7, 'g', 70), (8, 'h', 80), (9, 'i', 90);
SELECT * FROM ins_t WHERE id >= 7 ORDER BY id;

-- INSERT ... SELECT
CREATE TABLE ins_src (id INTEGER, name TEXT, val INTEGER);
INSERT INTO ins_src VALUES (100, 'src1', 1000);
INSERT INTO ins_src VALUES (101, 'src2', 2000);
INSERT INTO ins_t SELECT * FROM ins_src;
SELECT * FROM ins_t WHERE id >= 100 ORDER BY id;

DROP TABLE ins_src;
DROP TABLE ins_t;
