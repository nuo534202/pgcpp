-- types_bool.sql — Boolean type: TRUE/FALSE/NULL and logical operators.

SELECT TRUE AS t;
SELECT FALSE AS f;
SELECT TRUE::BOOLEAN AS t_cast;
SELECT FALSE::BOOLEAN AS f_cast;
SELECT 't'::BOOLEAN AS t_str;
SELECT 'f'::BOOLEAN AS f_str;
SELECT 'true'::BOOLEAN AS t_full;
SELECT 'false'::BOOLEAN AS f_full;

-- Logical operators
SELECT TRUE AND TRUE AS tt;
SELECT TRUE AND FALSE AS tf;
SELECT FALSE AND FALSE AS ff;
SELECT TRUE OR FALSE AS or_tf;
SELECT FALSE OR FALSE AS or_ff;
SELECT NOT TRUE AS not_t;
SELECT NOT FALSE AS not_f;

-- NULL in boolean logic (3-valued)
SELECT NULL::BOOLEAN AS null_bool;
SELECT TRUE AND NULL AS and_null;
SELECT FALSE AND NULL AS and_false_null;
SELECT TRUE OR NULL AS or_null;
SELECT FALSE OR NULL AS or_false_null;
SELECT NOT NULL::BOOLEAN AS not_null;

-- Comparison results are boolean
SELECT (1 = 1) AS eq_true;
SELECT (1 = 2) AS eq_false;
SELECT (1 < 2) AS lt_true;

-- Boolean in WHERE
CREATE TABLE bool_t (id INTEGER, flag BOOLEAN);
INSERT INTO bool_t VALUES (1, TRUE);
INSERT INTO bool_t VALUES (2, FALSE);
INSERT INTO bool_t VALUES (3, NULL);
SELECT id FROM bool_t WHERE flag;
SELECT id FROM bool_t WHERE flag = TRUE;
SELECT id FROM bool_t WHERE flag IS NOT TRUE;
SELECT COUNT(*) FROM bool_t WHERE flag IS NULL;
DROP TABLE bool_t;
