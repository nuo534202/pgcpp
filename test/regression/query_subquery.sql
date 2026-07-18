-- query_subquery.sql — Subqueries: scalar, IN, EXISTS, correlated.
--
-- Tests subquery decorrelation, scalar subquery evaluation, and
-- semi/anti-join transformation in the optimizer.

CREATE TABLE sq_dept (id INTEGER, name TEXT);
CREATE TABLE sq_emp (id INTEGER, dept_id INTEGER, salary INTEGER);

INSERT INTO sq_dept VALUES (1, 'Eng');
INSERT INTO sq_dept VALUES (2, 'Sales');
INSERT INTO sq_dept VALUES (3, 'HR');

INSERT INTO sq_emp VALUES (1, 1, 5000);
INSERT INTO sq_emp VALUES (2, 1, 6000);
INSERT INTO sq_emp VALUES (3, 2, 4500);
INSERT INTO sq_emp VALUES (4, 2, 5500);
INSERT INTO sq_emp VALUES (5, 3, 4000);

-- Scalar subquery in SELECT
SELECT id, salary, (SELECT MAX(salary) FROM sq_emp) AS max_sal FROM sq_emp ORDER BY id;

-- Scalar subquery in WHERE
SELECT * FROM sq_emp WHERE salary = (SELECT MAX(salary) FROM sq_emp);
SELECT * FROM sq_emp WHERE salary > (SELECT AVG(salary) FROM sq_emp) ORDER BY id;

-- IN subquery
SELECT * FROM sq_emp WHERE dept_id IN (SELECT id FROM sq_dept WHERE name = 'Eng') ORDER BY id;

-- NOT IN subquery
SELECT * FROM sq_emp WHERE dept_id NOT IN (SELECT id FROM sq_dept WHERE name = 'HR') ORDER BY id;

-- EXISTS subquery (correlated)
SELECT * FROM sq_emp e WHERE EXISTS (
    SELECT 1 FROM sq_dept d WHERE d.id = e.dept_id AND d.name = 'Eng'
) ORDER BY e.id;

-- NOT EXISTS subquery (correlated)
SELECT * FROM sq_emp e WHERE NOT EXISTS (
    SELECT 1 FROM sq_dept d WHERE d.id = e.dept_id AND d.name = 'HR'
) ORDER BY e.id;

-- Subquery in FROM (derived table)
SELECT t.dept_id, t.avg_sal
FROM (SELECT dept_id, AVG(salary) AS avg_sal FROM sq_emp GROUP BY dept_id) t
ORDER BY t.dept_id;

-- Correlated scalar subquery
SELECT e.id, e.salary,
       (SELECT AVG(salary) FROM sq_emp e2 WHERE e2.dept_id = e.dept_id) AS dept_avg
FROM sq_emp e
ORDER BY e.id;

DROP TABLE sq_emp;
DROP TABLE sq_dept;
