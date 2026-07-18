-- txn_basic.sql — Transaction control: BEGIN, COMMIT, ROLLBACK.
--
-- Tests transaction semantics: visibility of uncommitted changes, rollback,
-- and commit durability. Critical for MVCC correctness (report section 1.6).

-- Basic COMMIT
BEGIN;
CREATE TABLE txn_t1 (id INTEGER);
INSERT INTO txn_t1 VALUES (1);
INSERT INTO txn_t1 VALUES (2);
SELECT * FROM txn_t1 ORDER BY id;
COMMIT;
SELECT * FROM txn_t1 ORDER BY id;
DROP TABLE txn_t1;

-- Basic ROLLBACK
BEGIN;
CREATE TABLE txn_t2 (id INTEGER);
INSERT INTO txn_t2 VALUES (1);
SELECT * FROM txn_t2;
ROLLBACK;
SELECT * FROM txn_t2;

-- ROLLBACK to SAVEPOINT
BEGIN;
CREATE TABLE txn_t3 (id INTEGER);
INSERT INTO txn_t3 VALUES (1);
SAVEPOINT sp1;
INSERT INTO txn_t3 VALUES (2);
SELECT * FROM txn_t3 ORDER BY id;
ROLLBACK TO sp1;
SELECT * FROM txn_t3 ORDER BY id;
COMMIT;
SELECT * FROM txn_t3 ORDER BY id;
DROP TABLE txn_t3;

-- Nested transaction behavior
CREATE TABLE txn_t4 (id INTEGER);
BEGIN;
INSERT INTO txn_t4 VALUES (1);
SAVEPOINT sp1;
INSERT INTO txn_t4 VALUES (2);
SAVEPOINT sp2;
INSERT INTO txn_t4 VALUES (3);
SELECT * FROM txn_t4 ORDER BY id;
ROLLBACK TO sp2;
SELECT * FROM txn_t4 ORDER BY id;
COMMIT;
SELECT * FROM txn_t4 ORDER BY id;
DROP TABLE txn_t4;
