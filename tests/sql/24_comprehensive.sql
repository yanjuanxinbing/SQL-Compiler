-- 24_comprehensive.sql
-- 综合大场景：电商订单 + 用户 + 商品全链路查询

CREATE TABLE `users`(
    id INT PRIMARY KEY,
    name VARCHAR(50) NOT NULL,
    email VARCHAR(100),
    city VARCHAR(50)
);

CREATE TABLE `products`(
    id INT PRIMARY KEY,
    name VARCHAR(100),
    price FLOAT,
    stock INT
);

CREATE TABLE `orders`(
    id INT PRIMARY KEY,
    user_id INT,
    product_id INT,
    qty INT,
    amount FLOAT
);

-- 插入用户
INSERT INTO `users` VALUES
    (1, 'Alice',   'alice@x.com',  'Beijing'),
    (2, 'Bob',     'bob@x.com',    'Shanghai'),
    (3, 'Charlie', 'c@x.com',      'Beijing'),
    (4, 'David',   'david@x.com',  'Shenzhen'),
    (5, 'Eve',     'eve@x.com',    'Shanghai');

-- 插入商品
INSERT INTO `products` VALUES
    (101, 'Laptop',   6999.0, 50),
    (102, 'Phone',    3999.0, 200),
    (103, 'Tablet',   2999.0, 100),
    (104, 'Headphone', 299.0, 500),
    (105, 'Mouse',     99.0, 1000);

-- 插入订单
INSERT INTO `orders` VALUES
    (1001, 1, 101, 1, 6999.0),
    (1002, 1, 104, 2,  598.0),
    (1003, 2, 102, 1, 3999.0),
    (1004, 2, 105, 3,  297.0),
    (1005, 3, 103, 2, 5998.0),
    (1006, 4, 101, 1, 6999.0),
    (1007, 5, 104, 1,  299.0);

-- 查询 1：每个用户的订单总额
SELECT u.name, SUM(o.amount) AS total
FROM `users` u
INNER JOIN `orders` o ON u.id = o.user_id
GROUP BY u.name
ORDER BY total DESC;

-- 查询 2：每个城市的用户数
SELECT city, COUNT(*) AS cnt
FROM `users`
GROUP BY city
ORDER BY cnt DESC;

-- 查询 3：商品销售排行
SELECT p.name, SUM(o.qty) AS sold_qty, SUM(o.amount) AS revenue
FROM `products` p
INNER JOIN `orders` o ON p.id = o.product_id
GROUP BY p.name
ORDER BY revenue DESC;

-- 查询 4：用户的订单 + 商品 + 城市
SELECT u.name AS user, u.city, p.name AS product, o.qty, o.amount
FROM `users` u
INNER JOIN `orders` o    ON u.id = o.user_id
INNER JOIN `products` p  ON o.product_id = p.id
ORDER BY u.city, o.amount DESC;

-- 查询 5：北京用户的总消费
SELECT u.city, COUNT(*) AS cnt, SUM(o.amount) AS total
FROM `users` u
INNER JOIN `orders` o ON u.id = o.user_id
WHERE u.city = 'Beijing'
GROUP BY u.city;

-- 查询 6：销量 > 1 的商品
SELECT p.name, SUM(o.qty) AS sold
FROM `products` p
INNER JOIN `orders` o ON p.id = o.product_id
GROUP BY p.name
HAVING SUM(o.qty) > 1
ORDER BY sold DESC;

-- 查询 7：每个用户订单数（LEFT JOIN 含没下单的用户）
SELECT u.name, COUNT(o.id) AS order_cnt, COALESCE(SUM(o.amount), 0) AS total
FROM `users` u
LEFT JOIN `orders` o ON u.id = o.user_id
GROUP BY u.name
ORDER BY total DESC;

-- 查询 8：更新订单 1002 数量 +1
UPDATE `orders` SET qty = qty + 1 WHERE id = 1002;
SELECT * FROM `orders` WHERE id = 1002;

-- 查询 9：删除金额小于 300 的订单
DELETE FROM `orders` WHERE amount < 300;
SELECT * FROM `orders` ORDER BY id;

exit;