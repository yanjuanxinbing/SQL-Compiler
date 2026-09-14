-- 33_window.sql
-- 窗口函数（OVER 子句）：ROW_NUMBER / RANK / DENSE_RANK / 聚合 OVER / LAG / LEAD / NTILE
-- 窗口函数保留每行的明细，同时对一组相关行计算聚合。

CREATE TABLE emp_score(
    id INT,
    dept VARCHAR,
    name VARCHAR,
    salary FLOAT
);

INSERT INTO emp_score VALUES
    (1, 'IT',  'Alice',   9000.0),
    (2, 'IT',  'Bob',     8500.0),
    (3, 'IT',  'Charlie', 8500.0),
    (4, 'IT',  'David',   7000.0),
    (5, 'HR',  'Eve',     6000.0),
    (6, 'HR',  'Frank',   5500.0),
    (7, 'HR',  'Grace',   5500.0),
    (8, 'FIN', 'Henry',   7500.0);

-- ROW_NUMBER：按部门内薪资降序编号
SELECT id, dept, name, salary,
       ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary DESC) AS rn
FROM emp_score;

-- RANK：并列后跳号
SELECT id, dept, name, salary,
       RANK() OVER (PARTITION BY dept ORDER BY salary DESC) AS rk
FROM emp_score;

-- DENSE_RANK：并列后不跳号
SELECT id, dept, name, salary,
       DENSE_RANK() OVER (PARTITION BY dept ORDER BY salary DESC) AS drk
FROM emp_score;

-- NTILE：把分区切成 N 桶
SELECT id, dept, name, salary,
       NTILE(3) OVER (PARTITION BY dept ORDER BY salary DESC) AS bucket
FROM emp_score;

-- 聚合 OVER：每行附带给定分组的合计 / 平均
SELECT id, dept, name, salary,
       SUM(salary)   OVER (PARTITION BY dept) AS dept_total,
       AVG(salary)   OVER (PARTITION BY dept) AS dept_avg,
       COUNT(*)      OVER (PARTITION BY dept) AS dept_cnt,
       MIN(salary)   OVER (PARTITION BY dept) AS dept_min,
       MAX(salary)   OVER (PARTITION BY dept) AS dept_max
FROM emp_score;

-- LAG / LEAD：访问同一分区内的前后行
SELECT id, dept, name, salary,
       LAG(salary, 1, 0)  OVER (PARTITION BY dept ORDER BY salary) AS prev_salary,
       LEAD(salary, 1, 0) OVER (PARTITION BY dept ORDER BY salary) AS next_salary
FROM emp_score;

-- FIRST_VALUE / LAST_VALUE：窗口首尾值
SELECT id, dept, name, salary,
       FIRST_VALUE(name) OVER (PARTITION BY dept ORDER BY salary DESC) AS top_earner,
       LAST_VALUE(name)  OVER (PARTITION BY dept ORDER BY salary DESC
                                ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) AS bottom_earner
FROM emp_score;

-- PERCENT_RANK / CUME_DIST：相对位置
SELECT id, dept, name, salary,
       PERCENT_RANK() OVER (PARTITION BY dept ORDER BY salary) AS pct_rank,
       CUME_DIST()    OVER (PARTITION BY dept ORDER BY salary) AS cume_dist
FROM emp_score;

-- 命名窗口：WINDOW 子句避免重复
SELECT id, dept, name, salary,
       ROW_NUMBER() OVER w AS rn,
       RANK()       OVER w AS rk
FROM emp_score
WINDOW w AS (PARTITION BY dept ORDER BY salary DESC);

-- 全局窗口（无 PARTITION）：全表排名
SELECT id, name, salary,
       ROW_NUMBER() OVER (ORDER BY salary DESC) AS global_rn
FROM emp_score;

-- 窗口函数 + WHERE / HAVING 联合
SELECT * FROM (
    SELECT id, dept, name, salary,
           RANK() OVER (PARTITION BY dept ORDER BY salary DESC) AS rk
    FROM emp_score
) AS ranked
WHERE rk <= 2;

-- 窗口函数 + 聚合（混合）
SELECT dept, COUNT(*) AS cnt,
       MAX(MAX(salary)) OVER (PARTITION BY dept) AS max_in_dept
FROM emp_score
GROUP BY dept;

exit;