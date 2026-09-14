-- 05_join.sql
-- JOIN 测试：INNER JOIN（含表别名、ON 条件）

CREATE TABLE users(id INT, name VARCHAR, dept_id INT);
CREATE TABLE departments(id INT, dept_name VARCHAR);
CREATE TABLE orders(id INT, user_id INT, amount FLOAT);

INSERT INTO users VALUES (1, 'Alice', 10);
INSERT INTO users VALUES (2, 'Bob', 20);
INSERT INTO users VALUES (3, 'Charlie', 10);
INSERT INTO users VALUES (4, 'David', 30);

INSERT INTO departments VALUES (10, 'IT');
INSERT INTO departments VALUES (20, 'HR');
INSERT INTO departments VALUES (30, 'Sales');

INSERT INTO orders VALUES (1001, 1, 250.0);
INSERT INTO orders VALUES (1002, 1, 150.0);
INSERT INTO orders VALUES (1003, 2, 300.0);
INSERT INTO orders VALUES (1004, 4, 100.0);
INSERT INTO orders VALUES (1005, 4, 200.0);

-- 简单 JOIN
SELECT u.name, d.dept_name
FROM users u
INNER JOIN departments d ON u.dept_id = d.id;

-- 链式 JOIN
SELECT u.name, d.dept_name, o.amount
FROM users u
INNER JOIN departments d ON u.dept_id = d.id
INNER JOIN orders o ON u.id = o.user_id;

-- 限定列引用
SELECT users.name, departments.dept_name
FROM users
INNER JOIN departments ON users.dept_id = departments.id;

-- 带 WHERE 的 JOIN
SELECT u.name, d.dept_name
FROM users u
INNER JOIN departments d ON u.dept_id = d.id
WHERE d.dept_name = 'IT';

-- 聚合 + JOIN
SELECT d.dept_name, COUNT(o.id) AS order_count, SUM(o.amount) AS total
FROM users u
INNER JOIN departments d ON u.dept_id = d.id
INNER JOIN orders o ON u.id = o.user_id
GROUP BY d.dept_name
ORDER BY total DESC;

exit;
