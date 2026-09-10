-- 07_alias.sql
-- 别名测试：列别名（AS/隐式）、表别名

CREATE TABLE emp(id INT, name VARCHAR, salary FLOAT, dept VARCHAR);

INSERT INTO emp VALUES (1, 'Alice', 5000.0, 'IT');
INSERT INTO emp VALUES (2, 'Bob', 6000.0, 'HR');
INSERT INTO emp VALUES (3, 'Charlie', 7000.0, 'IT');

-- 显式 AS 别名
SELECT id AS emp_id, name AS emp_name, salary AS monthly FROM emp;

-- 隐式别名（无 AS）
SELECT id emp_id, name emp_name, salary monthly FROM emp;

-- 表别名
SELECT e.id, e.name, e.salary FROM emp e;

-- 表别名 + 列别名
SELECT e.id AS emp_id, e.name AS emp_name, e.salary AS monthly
FROM emp e
WHERE e.dept = 'IT';

-- JOIN + 双表别名
CREATE TABLE dept(id INT, name VARCHAR);
INSERT INTO dept VALUES (10, 'IT'), (20, 'HR');
SELECT e.name AS emp, d.name AS department
FROM emp e
INNER JOIN dept d ON e.dept_id = d.id;

exit;
