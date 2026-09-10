-- 01_ddl.sql
-- DDL 测试：CREATE / DROP TABLE、各种类型、约束

CREATE TABLE users(id INT PRIMARY KEY, name VARCHAR(50), email VARCHAR(100) NOT NULL, age INT);
CREATE TABLE products(
    id INT PRIMARY KEY,
    name VARCHAR(100),
    price FLOAT,
    description TEXT,
    stock BIGINT
);
CREATE TABLE IF NOT EXISTS logs(id INT, message VARCHAR(255));

INSERT INTO users VALUES (1, 'Alice', 'alice@x.com', 20);
INSERT INTO products VALUES (101, 'Widget', 9.99, 'A small widget', 1000);
INSERT INTO logs VALUES (1, 'system started');

-- 验证表都能正常 SELECT
SELECT * FROM users;
SELECT * FROM products;
SELECT * FROM logs;

-- DROP 测试
DROP TABLE logs;
CREATE TABLE logs(id INT, message VARCHAR(255));
INSERT INTO logs VALUES (2, 'system restarted');
SELECT * FROM logs;

exit;
