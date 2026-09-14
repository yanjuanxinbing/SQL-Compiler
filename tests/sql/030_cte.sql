-- 31_cte.sql
-- 公共表表达式（CTE）：WITH ... AS (...)
-- 用于把复杂查询拆分为多步，提高可读性与可复用性。

CREATE TABLE sales(
    id INT,
    region VARCHAR,
    product VARCHAR,
    amount FLOAT,
    qty INT
);

INSERT INTO sales VALUES
    (1,  'East', 'Apple',   100.0, 2),
    (2,  'East', 'Banana',   50.0, 5),
    (3,  'West', 'Apple',   200.0, 4),
    (4,  'West', 'Cherry',  150.0, 1),
    (5,  'South','Apple',   300.0, 6),
    (6,  'South','Donut',    80.0, 3),
    (7,  'East', 'Egg',      20.0,10);

-- 单个 CTE
WITH regional AS (
    SELECT region, SUM(amount) AS total
    FROM sales
    GROUP BY region
)
SELECT * FROM regional WHERE total >= 200;

-- CTE 在 WHERE 子句中被引用
WITH big_sales AS (
    SELECT * FROM sales WHERE amount >= 100
)
SELECT region, COUNT(*) AS cnt FROM big_sales GROUP BY region;

-- 多个 CTE（逗号分隔）
WITH
    east AS (SELECT * FROM sales WHERE region = 'East'),
    west AS (SELECT * FROM sales WHERE region = 'West')
SELECT
    (SELECT SUM(amount) FROM east)  AS east_total,
    (SELECT SUM(amount) FROM west)  AS west_total;

-- CTE 内再做聚合 + JOIN
WITH product_stats AS (
    SELECT product, SUM(amount) AS total, SUM(qty) AS units
    FROM sales
    GROUP BY product
),
top AS (
    SELECT * FROM product_stats ORDER BY total DESC LIMIT 2
)
SELECT * FROM top;

-- CTE 链式引用（前一个 CTE 可被后一个引用）
WITH base AS (
    SELECT region, product, amount FROM sales
),
enriched AS (
    SELECT region, amount, amount * 1.1 AS taxed FROM base WHERE region = 'East'
)
SELECT region, SUM(taxed) AS east_with_tax FROM enriched GROUP BY region;

-- CTE 与子查询的等价形式
WITH avg_amt AS (
    SELECT AVG(amount) AS a FROM sales
)
SELECT region, amount
FROM sales, avg_amt
WHERE amount > a;

-- 递归 CTE（WITH RECURSIVE）—— 组织结构树
CREATE TABLE org(
    emp_id INT,
    name VARCHAR,
    mgr_id INT
);

INSERT INTO org VALUES
    (1, 'CEO',    NULL),
    (2, 'VP1',    1),
    (3, 'VP2',    1),
    (4, 'Dir1',   2),
    (5, 'Dir2',   2),
    (6, 'Mgr1',   4);

-- 递归 CTE：从根节点出发，逐层展开下属
WITH RECURSIVE hierarchy AS (
    SELECT emp_id, name, mgr_id, 0 AS depth
    FROM org WHERE mgr_id IS NULL
    UNION ALL
    SELECT o.emp_id, o.name, o.mgr_id, h.depth + 1
    FROM org o JOIN hierarchy h ON o.mgr_id = h.emp_id
)
SELECT * FROM hierarchy ORDER BY depth, emp_id;

exit;