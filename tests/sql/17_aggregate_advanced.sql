-- 17_aggregate_advanced.sql
-- 高级聚合：HAVING、多层过滤、聚合后再过滤

CREATE TABLE orders(
    id INT,
    region VARCHAR,
    product VARCHAR,
    amount FLOAT,
    qty INT
);

INSERT INTO orders VALUES
    (1,  'East',  'Apple',  100.0, 2),
    (2,  'East',  'Banana', 50.0,  5),
    (3,  'West',  'Apple',  200.0, 4),
    (4,  'West',  'Cherry', 150.0, 1),
    (5,  'South', 'Apple',  300.0, 6),
    (6,  'South', 'Donut',  80.0,  3),
    (7,  'East',  'Egg',    20.0,  10),
    (8,  'West',  'Fig',    90.0,  2);

-- 仅 HAVING 限制数量
SELECT region, COUNT(*) AS cnt
FROM orders
GROUP BY region
HAVING COUNT(*) >= 2;

-- HAVING 限制总金额
SELECT region, SUM(amount) AS total
FROM orders
GROUP BY region
HAVING SUM(amount) > 200;

-- WHERE + GROUP BY + HAVING + ORDER BY
SELECT region, COUNT(*) AS cnt, AVG(amount) AS avg_amt
FROM orders
WHERE product != 'Egg'
GROUP BY region
HAVING AVG(amount) > 100
ORDER BY avg_amt DESC;

-- HAVING 中引用别名
SELECT region, COUNT(*) AS cnt
FROM orders
GROUP BY region
HAVING cnt > 1
ORDER BY cnt;

-- 多列分组 + HAVING
SELECT region, product, COUNT(*) AS cnt, SUM(qty) AS total_qty
FROM orders
GROUP BY region, product
HAVING SUM(qty) >= 4
ORDER BY region, total_qty DESC;

-- 仅 SELECT HAVING 列：分组后求 max
SELECT region, MAX(amount) AS top_sale
FROM orders
GROUP BY region
HAVING MAX(amount) >= 150
ORDER BY top_sale DESC;

-- LIMIT 限制最终聚合结果数
SELECT region, SUM(amount) AS total
FROM orders
GROUP BY region
ORDER BY total DESC
LIMIT 2;

exit;