-- 20_complex.sql
-- 复杂查询：多层过滤 + 多列分组 + HAVING + 多表 JOIN + ORDER BY + LIMIT

CREATE TABLE dept(id INT, name VARCHAR, region VARCHAR);
CREATE TABLE emp(id INT, name VARCHAR, dept_id INT, salary FLOAT, bonus FLOAT);
CREATE TABLE project(id INT, lead_emp_id INT, budget FLOAT);

INSERT INTO dept VALUES
    (10, 'IT',      'East'),
    (20, 'HR',      'East'),
    (30, 'Sales',   'West'),
    (40, 'Research','West');

INSERT INTO emp VALUES
    (1, 'Alice',   10, 8000.0, 1000.0),
    (2, 'Bob',     10, 9500.0, 1500.0),
    (3, 'Charlie', 20, 6000.0,  500.0),
    (4, 'David',   30, 7500.0,  800.0),
    (5, 'Eve',     30, 8200.0,  900.0),
    (6, 'Frank',   40, 9000.0, 2000.0),
    (7, 'Grace',   10, 7800.0,  700.0);

INSERT INTO project VALUES
    (1001, 1, 50000.0),
    (1002, 4, 30000.0),
    (1003, 6, 80000.0);

-- 部门平均薪资（含奖金），且员工人数 > 1
SELECT d.name, COUNT(*) AS cnt, AVG(e.salary + e.bonus) AS avg_total
FROM dept  d
INNER JOIN emp e ON d.id = e.dept_id
GROUP BY d.name
HAVING COUNT(*) > 1
ORDER BY avg_total DESC;

-- 各 region 的项目预算总和
SELECT d.region, SUM(p.budget) AS total_budget, COUNT(*) AS project_cnt
FROM dept     d
INNER JOIN emp     e ON d.id = e.dept_id
INNER JOIN project p ON e.id = p.lead_emp_id
GROUP BY d.region
ORDER BY total_budget DESC;

-- 高薪员工 + 部门信息，按薪资 + 奖金排序
SELECT e.name, d.name AS dept, e.salary + e.bonus AS total_comp
FROM emp e
INNER JOIN dept d ON e.dept_id = d.id
WHERE e.salary + e.bonus > 8000
ORDER BY total_comp DESC
LIMIT 5;

-- 部门员工数 = 0 的部门（LEFT JOIN 找空部门）
SELECT d.name
FROM dept d
LEFT JOIN emp e ON d.id = e.dept_id
WHERE e.id IS NULL;

-- 统计各 region 各部门的员工数
SELECT d.region, d.name, COUNT(e.id) AS cnt
FROM dept d
LEFT JOIN emp e ON d.id = e.dept_id
GROUP BY d.region, d.name
ORDER BY d.region, cnt DESC;

exit;