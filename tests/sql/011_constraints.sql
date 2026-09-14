-- 12_constraints.sql
-- 列约束：PRIMARY KEY / NOT NULL，多列各种组合

-- 单 PK
CREATE TABLE pk_one(id INT PRIMARY KEY, name VARCHAR, age INT);

-- 多个 PK + NOT NULL 组合
CREATE TABLE pk_multi(
    id INT PRIMARY KEY,
    email VARCHAR(100) NOT NULL,
    nickname VARCHAR(50)
);

-- 多个 PRIMARY KEY 列
CREATE TABLE pk_combo(
    a INT,
    b INT,
    val VARCHAR,
    PRIMARY KEY(a, b)
);

-- VARCHAR 带长度
CREATE TABLE sized(
    id INT,
    short_name VARCHAR(10),
    long_name VARCHAR(1000),
    note TEXT
);

-- 含 FLOAT 列
CREATE TABLE measure(
    id INT PRIMARY KEY,
    weight FLOAT,
    height FLOAT NOT NULL
);

-- 插入合法数据
INSERT INTO pk_one VALUES (1, 'Alice', 20);
INSERT INTO pk_one VALUES (2, 'Bob', 25);
INSERT INTO pk_multi VALUES (1, 'a@x.com', 'ali');
INSERT INTO pk_combo VALUES (1, 1, 'one-one');
INSERT INTO pk_combo VALUES (1, 2, 'one-two');
INSERT INTO sized VALUES (1, 'abc', 'long...', 'note');
INSERT INTO measure VALUES (1, 60.5, 170.0);

-- 查询
SELECT * FROM pk_one;
SELECT * FROM pk_combo;
SELECT * FROM sized;
SELECT * FROM measure;

exit;