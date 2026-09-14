-- 30_subquery.sql
-- 子查询：标量子查询 / IN 子查询 / EXISTS / FROM 子查询 / 相关子查询

CREATE TABLE customers(
    id INT,
    name VARCHAR,
    city VARCHAR
);

CREATE TABLE orders(
    id INT,
    cust_id INT,
    amount FLOAT
);

INSERT INTO customers VALUES
    (1, 'Alice',   'BJ'),
    (2, 'Bob',     'SH'),
    (3, 'Charlie', 'BJ'),
    (4, 'David',   'GZ'),
    (5, 'Eve',     'SH');

INSERT INTO orders VALUES
    (1, 1, 100.0),
    (2, 1, 50.0),
    (3, 2, 200.0),
    (4, 3, 80.0),
    (5, 3, 60.0),
    (6, 5, 300.0);

-- 标量子查询：SELECT 列表中的单值子查询
SELECT id, name,
       (SELECT SUM(amount) FROM orders WHERE cust_id = customers.id) AS total
FROM customers;

-- 标量子查询：WHERE 中比较
SELECT name FROM customers
WHERE id = (SELECT cust_id FROM orders ORDER BY amount DESC LIMIT 1);

-- IN 子查询：WHERE col IN (SELECT ...)
SELECT name FROM customers
WHERE id IN (SELECT cust_id FROM orders WHERE amount >= 100);

-- NOT IN 子查询
SELECT name FROM customers
WHERE id NOT IN (SELECT cust_id FROM orders WHERE amount >= 200);

-- EXISTS 子查询
SELECT name FROM customers c
WHERE EXISTS (SELECT 1 FROM orders o WHERE o.cust_id = c.id);

-- NOT EXISTS 子查询
SELECT name FROM customers c
WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.cust_id = c.id AND o.amount > 100);

-- FROM 子查询：派生表
SELECT sub.cust_id, sub.cnt
FROM (SELECT cust_id, COUNT(*) AS cnt FROM orders GROUP BY cust_id) AS sub
WHERE sub.cnt >= 2;

-- 相关子查询：每行计算其订单总额
SELECT name,
       (SELECT COUNT(*) FROM orders o WHERE o.cust_id = c.id) AS order_cnt
FROM customers c;

-- ANY / ALL 子查询（部分方言）
SELECT name FROM customers
WHERE id > ANY (SELECT cust_id FROM orders WHERE amount >= 100);

-- 子查询嵌套
SELECT name FROM customers
WHERE id IN (
    SELECT cust_id FROM orders
    WHERE amount > (SELECT AVG(amount) FROM orders)
);

exit;