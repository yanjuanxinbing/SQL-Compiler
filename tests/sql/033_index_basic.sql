-- 34_index_basic.sql
-- 索引基础：主键自动建索引、二级索引、唯一索引、索引点查与范围查

CREATE TABLE idx_users(id INT PRIMARY KEY, name VARCHAR(50) NOT NULL, age INT);
INSERT INTO idx_users VALUES (1, 'Alice', 20);
INSERT INTO idx_users VALUES (2, 'Bob', 25);
INSERT INTO idx_users VALUES (3, 'Carl', 30);
INSERT INTO idx_users VALUES (4, 'Dave', 35);
INSERT INTO idx_users VALUES (5, 'Eve', 40);

-- 主键索引（CREATE TABLE 时自动建立）上的等值与范围查询
SELECT * FROM idx_users WHERE id = 3;
SELECT * FROM idx_users WHERE id >= 4;
SELECT * FROM idx_users WHERE id > 1 AND id <= 3;
SELECT * FROM idx_users WHERE id BETWEEN 2 AND 4;
SELECT * FROM idx_users WHERE id < 2;
-- 常量写在左侧
SELECT * FROM idx_users WHERE 5 = id;
-- 索引区间 + 残余谓词（age 无索引，回表后再判）
SELECT * FROM idx_users WHERE id >= 2 AND age > 30;
-- 排序后输出，确保与访问路径无关
SELECT * FROM idx_users WHERE id > 0 ORDER BY id;
SELECT COUNT(*) FROM idx_users WHERE id > 2;

-- 二级索引（非唯一）
CREATE INDEX idx_users_age ON idx_users(age);
SELECT * FROM idx_users WHERE age = 30;
SELECT * FROM idx_users WHERE age >= 35 ORDER BY age;

-- 唯一二级索引
CREATE UNIQUE INDEX uq_users_name ON idx_users(name);
SELECT * FROM idx_users WHERE name = 'Bob';

-- 复合主键同样自动建索引
CREATE TABLE idx_pair(a INT, b INT, val VARCHAR(20), PRIMARY KEY(a, b));
INSERT INTO idx_pair VALUES (1, 1, 'one-one');
INSERT INTO idx_pair VALUES (1, 2, 'one-two');
INSERT INTO idx_pair VALUES (2, 1, 'two-one');
SELECT * FROM idx_pair ORDER BY a, b;

exit;
