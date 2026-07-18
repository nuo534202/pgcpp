-- query_join.sql — JOIN operations: INNER, LEFT, RIGHT, FULL, CROSS, self.
--
-- Tests join algorithms and column resolution across multiple tables.
-- Multi-table joins are a major gap in single-table benchmarks like ClickBench.

CREATE TABLE j_dept (dept_id INTEGER, dept_name TEXT);
CREATE TABLE j_emp (emp_id INTEGER, emp_name TEXT, dept_id INTEGER);
CREATE TABLE j_proj (proj_id INTEGER, proj_name TEXT, emp_id INTEGER);

INSERT INTO j_dept VALUES (1, 'Engineering');
INSERT INTO j_dept VALUES (2, 'Sales');
INSERT INTO j_dept VALUES (3, 'HR');
INSERT INTO j_dept VALUES (4, 'Marketing');

INSERT INTO j_emp VALUES (1, 'Alice', 1);
INSERT INTO j_emp VALUES (2, 'Bob', 1);
INSERT INTO j_emp VALUES (3, 'Carol', 2);
INSERT INTO j_emp VALUES (4, 'Dave', 2);
INSERT INTO j_emp VALUES (5, 'Eve', 5);

INSERT INTO j_proj VALUES (101, 'Alpha', 1);
INSERT INTO j_proj VALUES (102, 'Beta', 2);
INSERT INTO j_proj VALUES (103, 'Gamma', 9);

-- INNER JOIN (two tables)
SELECT j_emp.emp_name, j_dept.dept_name
FROM j_emp INNER JOIN j_dept ON j_emp.dept_id = j_dept.dept_id
ORDER BY j_emp.emp_id;

-- INNER JOIN with aliases
SELECT e.emp_name, d.dept_name
FROM j_emp e INNER JOIN j_dept d ON e.dept_id = d.dept_id
ORDER BY e.emp_id;

-- LEFT JOIN
SELECT e.emp_name, d.dept_name
FROM j_emp e LEFT JOIN j_dept d ON e.dept_id = d.dept_id
ORDER BY e.emp_id;

-- RIGHT JOIN
SELECT e.emp_name, d.dept_name
FROM j_emp e RIGHT JOIN j_dept d ON e.dept_id = d.dept_id
ORDER BY d.dept_id;

-- FULL OUTER JOIN
SELECT e.emp_name, d.dept_name
FROM j_emp e FULL OUTER JOIN j_dept d ON e.dept_id = d.dept_id
ORDER BY d.dept_id, e.emp_id;

-- CROSS JOIN
SELECT e.emp_name, d.dept_name
FROM j_emp e CROSS JOIN j_dept d
ORDER BY e.emp_id, d.dept_id;

-- Three-table join
SELECT e.emp_name, d.dept_name, p.proj_name
FROM j_emp e
INNER JOIN j_dept d ON e.dept_id = d.dept_id
INNER JOIN j_proj p ON p.emp_id = e.emp_id
ORDER BY e.emp_id;

-- LEFT JOIN with no matches (project with emp_id=9 has no employee)
SELECT p.proj_name, e.emp_name
FROM j_proj p LEFT JOIN j_emp e ON p.emp_id = e.emp_id
ORDER BY p.proj_id;

-- JOIN with WHERE filter
SELECT e.emp_name, d.dept_name
FROM j_emp e INNER JOIN j_dept d ON e.dept_id = d.dept_id
WHERE d.dept_name = 'Engineering'
ORDER BY e.emp_id;

-- JOIN with aggregate
SELECT d.dept_name, COUNT(e.emp_id) AS emp_count
FROM j_dept d LEFT JOIN j_emp e ON e.dept_id = d.dept_id
GROUP BY d.dept_name
ORDER BY d.dept_name;

DROP TABLE j_proj;
DROP TABLE j_emp;
DROP TABLE j_dept;
