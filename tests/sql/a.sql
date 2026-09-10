-- =====================================
-- 1. 创建用户表
-- =====================================

CREATE TABLE users (
    id INT,
    name VARCHAR,
    age INT,
    score FLOAT
);


-- =====================================
-- 2. 插入用户数据
-- =====================================

INSERT INTO users(id,name,age,score)
VALUES
(1,'Alice',20,88.5),
(2,'Bob',22,91.0),
(3,'Charlie',19,76.5),
(4,'David',25,95.5),
(5,'Eva',21,82.0);


-- 查询验证
SELECT *
FROM users;


-- 排序查询
SELECT id,name,score
FROM users
ORDER BY score DESC;


-- 限制数量
SELECT *
FROM users
ORDER BY age ASC
LIMIT 3;



-- =====================================
-- 3. 修改数据 UPDATE
-- =====================================

UPDATE users
SET score = 93.5
WHERE id = 2;


-- 修改后查询

SELECT *
FROM users
WHERE id = 2;



-- =====================================
-- 4. 批量修改
-- =====================================

UPDATE users
SET age = 23
WHERE score > 85;


-- 修改后排序查询

SELECT id,name,age,score
FROM users
ORDER BY score DESC
LIMIT 5;



-- =====================================
-- 5. 创建商品表
-- =====================================

CREATE TABLE product (
    id INT,
    name VARCHAR,
    price FLOAT,
    stock INT
);



-- =====================================
-- 6. 商品数据插入
-- =====================================

INSERT INTO product(id,name,price,stock)
VALUES
(1,'Laptop',6999.0,10),
(2,'Phone',3999.0,20),
(3,'Tablet',2999.0,15),
(4,'Mouse',99.0,100),
(5,'Keyboard',199.0,50);



-- 查询商品

SELECT *
FROM product;


-- 价格排序

SELECT name,price
FROM product
ORDER BY price DESC;


-- 低价商品

SELECT *
FROM product
WHERE price < 3000
ORDER BY price ASC
LIMIT 2;



-- =====================================
-- 7. 删除数据 DELETE
-- =====================================

DELETE FROM users
WHERE age < 20;



-- 删除后检查

SELECT *
FROM users
ORDER BY age ASC;



-- 删除商品

DELETE FROM product
WHERE stock > 80;



-- 删除后查询

SELECT *
FROM product;



-- =====================================
-- 8. 更多随机条件测试
-- =====================================

INSERT INTO users(id,name,age,score)
VALUES
(6,'Frank',30,90.5),
(7,'Grace',28,87.5);



SELECT id,name
FROM users
WHERE score >= 90
ORDER BY score DESC
LIMIT 3;



UPDATE users
SET score = 100
WHERE name = 'Grace';



SELECT *
FROM users
WHERE name = 'Grace';



DELETE FROM users
WHERE score < 80;



SELECT *
FROM users
ORDER BY score DESC
LIMIT 10;



-- =====================================
-- 9. 清理测试
-- =====================================

DROP TABLE product;

DROP TABLE users;