-- 27_case_scalar.sql
-- 标量表达式：CASE WHEN / COALESCE / NULLIF
-- CASE 用于把列值映射到另一组值；COALESCE 返回首个非 NULL；NULLIF 相等则返回 NULL。

CREATE TABLE emp(
    id INT,
    name VARCHAR,
    dept VARCHAR,
    salary FLOAT,
    bonus FLOAT
);

INSERT INTO emp VALUES
    (1, 'Alice',   'IT',  9000.0,  1000.0),
    (2, 'Bob',     'HR',  5500.0,  NULL),
    (3, 'Charlie', 'IT',  8000.0,  NULL),
    (4, 'David',   'HR',  6000.0,  500.0),
    (5, 'Eve',     'FIN', 7000.0,  NULL);

-- 简单 CASE：等值映射
SELECT id, name,
       CASE dept
           WHEN 'IT' THEN 'Engineering'
           WHEN 'HR' THEN 'People'
           ELSE 'Other'
       END AS dept_label
FROM emp;

-- 搜索式 CASE：范围判定
SELECT id, name,
       CASE
           WHEN salary >= 8000 THEN 'high'
           WHEN salary >= 6000 THEN 'mid'
           ELSE 'low'
       END AS salary_band
FROM emp;

-- CASE 在聚合内使用：把 NULL bonus 当作 0
SELECT dept, SUM(CASE WHEN bonus IS NULL THEN 0 ELSE bonus END) AS total_bonus
FROM emp
GROUP BY dept;

-- COALESCE：取首个非 NULL
SELECT id, name, COALESCE(bonus, 0.0) AS bonus_or_zero
FROM emp;

-- COALESCE 多个参数
SELECT id, name,
       COALESCE(bonus, salary * 0.1, 0.0) AS fallback
FROM emp;

-- NULLIF：相等返回 NULL，否则返回第一个
SELECT id,
       NULLIF(dept, 'HR') AS dept_or_null
FROM emp;

-- CASE 嵌套
SELECT id, name,
       CASE dept
           WHEN 'IT' THEN
               CASE WHEN salary >= 8500 THEN 'IT-senior' ELSE 'IT-junior' END
           ELSE 'non-IT'
       END AS tier
FROM emp;

-- GROUP BY 引用 SELECT-list 别名：SQL 标准允许 GROUP BY/HAVING/ORDER BY 引用同 SELECT
-- 块内已定义的别名（与 PostgreSQL / MySQL 一致）。
-- 修复前：GROUP BY tier 报「column not found: tier」；修复后 AggregateExecutor 按
-- 替换后的 CASE 表达式对每条输入行求 val，把 salary 落到对应 tier 桶里。
SELECT CASE
           WHEN salary >= 8000 THEN 'high'
           WHEN salary >= 6000 THEN 'mid'
           ELSE 'low'
       END AS tier,
       COUNT(*) AS n
FROM emp
GROUP BY tier
ORDER BY tier;

-- 复合别名：GROUP BY 同时使用列别名与 CASE 表达式
SELECT dept,
       CASE WHEN salary >= 8000 THEN 'S' ELSE 'J' END AS seniority,
       COUNT(*) AS cnt
FROM emp
GROUP BY dept, seniority
ORDER BY dept, seniority;

exit;