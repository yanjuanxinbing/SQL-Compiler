-- 13_outer_join.sql
-- LEFT JOIN / RIGHT JOIN：包含没有匹配行的边

CREATE TABLE customers(id INT, name VARCHAR);
CREATE TABLE orders(id INT, cust_id INT, amount FLOAT);

-- 4 个客户（id 3 没有订单；id 4 没有订单）
INSERT INTO customers VALUES
    (1, 'Alice'),
    (2, 'Bob'),
    (3, 'Charlie'),
    (4, 'David');

-- 只有 Alice / Bob 有订单
INSERT INTO orders VALUES
    (1001, 1, 250.0),
    (1002, 1, 150.0),
    (1003, 2, 300.0);

-- INNER JOIN：只显示 Alice / Bob
SELECT c.id, c.name, o.amount
FROM customers c
INNER JOIN orders o ON c.id = o.cust_id;

-- LEFT JOIN：显示所有客户（无订单者 amount 为 NULL）
SELECT c.id, c.name, o.amount
FROM customers c
LEFT JOIN orders o ON c.id = o.cust_id;

-- RIGHT JOIN：以订单为左侧（所有订单 + 客户）
SELECT c.id, c.name, o.amount
FROM customers c
RIGHT JOIN orders o ON c.id = o.cust_id;

-- LEFT JOIN + WHERE：找出没有订单的客户
SELECT c.id, c.name
FROM customers c
LEFT JOIN orders o ON c.id = o.cust_id
WHERE o.id IS NULL;

-- LEFT JOIN + 聚合：每位客户订单数
SELECT c.name, COUNT(o.id) AS cnt, SUM(o.amount) AS total
FROM customers c
LEFT JOIN orders o ON c.id = o.cust_id
GROUP BY c.name
ORDER BY cnt DESC;

exit;