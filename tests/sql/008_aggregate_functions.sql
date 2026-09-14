-- 09_aggregate_functions.sql
-- 聚合函数基础：COUNT / SUM / AVG / MIN / MAX 各种组合

CREATE TABLE orders(
    id INT,
    cust_id INT,
    product VARCHAR,
    qty INT,
    price FLOAT
);

INSERT INTO orders VALUES
    (1, 101, 'Apple',  3, 1.50),
    (2, 101, 'Banana', 5, 0.80),
    (3, 102, 'Apple',  2, 1.50),
    (4, 103, 'Cherry', 1, 5.00),
    (5, 102, 'Donut',  4, 2.00),
    (6, 101, 'Egg',    6, 0.30),
    (7, 104, 'Fig',    2, 4.50),
    (8, 104, 'Grape',  3, 2.20);

-- 单聚合：COUNT(*)
SELECT COUNT(*) FROM orders;

-- COUNT 特定列（语义上一般忽略 NULL）
SELECT COUNT(qty) FROM orders;

-- 总和 / 平均
SELECT SUM(qty) AS total_qty FROM orders;
SELECT AVG(price) AS avg_price FROM orders;

-- 极值
SELECT MIN(price) AS min_price FROM orders;
SELECT MAX(price) AS max_price FROM orders;

-- 同时多个聚合
SELECT
    COUNT(*)   AS n,
    SUM(qty)   AS total_qty,
    AVG(price) AS avg_price,
    MIN(price) AS min_price,
    MAX(price) AS max_price
FROM orders;

-- 仅选择部分列 + 聚合
SELECT cust_id, COUNT(*) FROM orders GROUP BY cust_id;

exit;