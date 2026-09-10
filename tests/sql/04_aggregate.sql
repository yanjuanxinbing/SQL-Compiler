-- 04_aggregate.sql
-- 聚合函数：COUNT / SUM / AVG / MIN / MAX、GROUP BY、HAVING

CREATE TABLE sales(id INT, dept VARCHAR, amount FLOAT, region VARCHAR);

INSERT INTO sales VALUES
    (1, 'IT', 1000.0, 'East'),
    (2, 'IT', 1500.0, 'West'),
    (3, 'IT', 800.0, 'East'),
    (4, 'HR', 600.0, 'East'),
    (5, 'HR', 700.0, 'West'),
    (6, 'Sales', 2000.0, 'East'),
    (7, 'Sales', 1800.0, 'West'),
    (8, 'IT', 1200.0, 'South');

-- 无分组聚合
SELECT COUNT(*) FROM sales;
SELECT COUNT(*), SUM(amount), AVG(amount), MIN(amount), MAX(amount) FROM sales;

-- 按部门分组
SELECT dept, COUNT(*) FROM sales GROUP BY dept;
SELECT dept, COUNT(*), SUM(amount) AS total FROM sales GROUP BY dept;

-- 多列分组
SELECT dept, region, COUNT(*), SUM(amount) FROM sales GROUP BY dept, region;

-- HAVING 过滤分组
SELECT dept, COUNT(*) FROM sales GROUP BY dept HAVING COUNT(*) >= 2;
SELECT dept, SUM(amount) AS total FROM sales GROUP BY dept HAVING SUM(amount) > 2000;

-- 组合查询
SELECT dept, COUNT(*) AS cnt, AVG(amount) AS avg_amt
FROM sales
WHERE region != 'South'
GROUP BY dept
HAVING COUNT(*) > 1
ORDER BY avg_amt DESC
LIMIT 5;

exit;
