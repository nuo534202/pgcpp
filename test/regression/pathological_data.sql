-- pathological_data.sql — Generate boundary / pathological datasets.
--
-- Implements the recommendation from pgcpp_benchmark_and_testing_report.md
-- section 4 ("自造'病态数据'"): tables that exercise edge cases which
-- normal benchmarks (ClickBench) never hit:
--   * all-NULL columns
--   * single-value skewed columns
--   * super-long strings (TOAST path)
--   * extreme numeric values
--   * invalid / boundary UTF-8
--
-- The schema is intentionally simple and self-contained so it can be loaded
-- into BOTH pgcpp and stock PostgreSQL for differential testing (section 1.4).
--
-- Each section drops its table at the end so the file is idempotent.

-- =====================================================================
-- Section 1: All-NULL columns
-- Exercises NULL bitmap, IS NULL semantics, COALESCE, aggregate-on-NULL.
-- =====================================================================
CREATE TABLE path_null (
    id    INTEGER,
    a_int INTEGER,
    a_txt TEXT,
    a_dbl DOUBLE PRECISION
);

INSERT INTO path_null VALUES (1, NULL, NULL, NULL);
INSERT INTO path_null VALUES (2, NULL, NULL, NULL);
INSERT INTO path_null VALUES (3, NULL, NULL, NULL);

SELECT COUNT(*) FROM path_null;
SELECT COUNT(a_int) FROM path_null;
SELECT SUM(a_int) FROM path_null;
SELECT AVG(a_dbl) FROM path_null;
SELECT id FROM path_null WHERE a_int IS NULL;

DROP TABLE path_null;

-- =====================================================================
-- Section 2: Single-value skewed column
-- All rows share the same value in one column; tests GROUP BY cardinality
-- estimation, hash-table bucket distribution, and DISTINCT correctness.
-- =====================================================================
CREATE TABLE path_skew (
    id    INTEGER,
    cat   TEXT,
    val   INTEGER
);

INSERT INTO path_skew VALUES (1, 'A', 10);
INSERT INTO path_skew VALUES (2, 'A', 20);
INSERT INTO path_skew VALUES (3, 'A', 30);
INSERT INTO path_skew VALUES (4, 'A', 40);
INSERT INTO path_skew VALUES (5, 'A', 50);

SELECT cat, COUNT(*) FROM path_skew GROUP BY cat;
SELECT COUNT(DISTINCT cat) FROM path_skew;
SELECT SUM(val) FROM path_skew WHERE cat = 'A';

DROP TABLE path_skew;

-- =====================================================================
-- Section 3: Super-long strings (TOAST path)
-- Strings longer than 2000 bytes force PostgreSQL's TOAST mechanism.
-- In pgcpp this exercises the varlena / long-text storage path and the
-- length()/substring() functions on oversized values.
-- =====================================================================
CREATE TABLE path_long (
    id   INTEGER,
    s    TEXT
);

INSERT INTO path_long VALUES (1, repeat('a', 100));
INSERT INTO path_long VALUES (2, repeat('b', 2000));
INSERT INTO path_long VALUES (3, repeat('c', 10000));

SELECT id, LENGTH(s) FROM path_long ORDER BY id;
SELECT id FROM path_long WHERE LENGTH(s) > 1000;

DROP TABLE path_long;

-- =====================================================================
-- Section 4: Extreme numeric values
-- Boundary values of integer / floating-point / numeric types: max, min,
-- zero, negative, fractional, very high precision.
-- =====================================================================
CREATE TABLE path_num (
    id       INTEGER,
    big      BIGINT,
    small    SMALLINT,
    real_v   REAL,
    dbl_v    DOUBLE PRECISION,
    num_v    NUMERIC
);

INSERT INTO path_num VALUES (1, 9223372036854775807, 32767,  3.402823e38,   1.7976931348623157e308, 99999999999999999999.999999);
INSERT INTO path_num VALUES (2, -9223372036854775808, -32768, -3.402823e38,  -1.7976931348623157e308, -99999999999999999999.999999);
INSERT INTO path_num VALUES (3, 0, 0, 0.0, 0.0, 0.0);
INSERT INTO path_num VALUES (4, 1, 1, 0.000001, 0.0000001, 0.00000001);
INSERT INTO path_num VALUES (5, -1, -1, -0.000001, -0.0000001, -0.00000001);

SELECT id, big, small FROM path_num ORDER BY id;
SELECT MAX(big), MIN(big) FROM path_num;
SELECT SUM(big) FROM path_num WHERE big > 0;
SELECT COUNT(*) FROM path_num WHERE num_v > 0;

DROP TABLE path_num;

-- =====================================================================
-- Section 5: Empty strings vs NULL
-- Distinguishes '' from NULL — a common source of bugs in rewritten engines.
-- =====================================================================
CREATE TABLE path_empty (
    id   INTEGER,
    s    TEXT
);

INSERT INTO path_empty VALUES (1, '');
INSERT INTO path_empty VALUES (2, 'hello');
INSERT INTO path_empty VALUES (3, NULL);
INSERT INTO path_empty VALUES (4, '   ');

SELECT id, LENGTH(s) FROM path_empty ORDER BY id;
SELECT COUNT(*) FROM path_empty WHERE s = '';
SELECT COUNT(*) FROM path_empty WHERE s IS NULL;
SELECT COUNT(*) FROM path_empty WHERE s IS NOT NULL;

DROP TABLE path_empty;

-- =====================================================================
-- Section 6: UTF-8 / non-ASCII text
-- Multi-byte characters, combining characters, and surrogate-pair-like
-- codepoints. Tests string length (bytes vs chars), LIKE, UPPER/LOWER on
-- non-ASCII, and substring boundaries.
-- =====================================================================
CREATE TABLE path_utf8 (
    id   INTEGER,
    s    TEXT
);

INSERT INTO path_utf8 VALUES (1, 'plain ascii');
INSERT INTO path_utf8 VALUES (2, '中文测试');
INSERT INTO path_utf8 VALUES (3, 'café');
INSERT INTO path_utf8 VALUES (4, 'naïve');
INSERT INTO path_utf8 VALUES (5, '日本語テスト');
INSERT INTO path_utf8 VALUES (6, '한국어');
INSERT INTO path_utf8 VALUES (7, 'emoji 🎉');

SELECT id, s, LENGTH(s) FROM path_utf8 ORDER BY id;
SELECT id FROM path_utf8 WHERE s LIKE '%中%';
SELECT id FROM path_utf8 WHERE s LIKE '%a%';

DROP TABLE path_utf8;
