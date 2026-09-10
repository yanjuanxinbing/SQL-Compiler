-- 08_types.sql
-- 数据类型测试：INT / FLOAT / VARCHAR / TEXT / BIGINT 类型强制转换

CREATE TABLE t(
    i INT,
    bi BIGINT,
    f FLOAT,
    d DOUBLE,
    v VARCHAR(50),
    txt TEXT
);

-- INT 列插入 INT 字面量
INSERT INTO t(i) VALUES (42);
INSERT INTO t(i) VALUES (-100);

-- INT 列插入 FLOAT 字面量（应自动取整）
INSERT INTO t(i) VALUES (3.7);
INSERT INTO t(i) VALUES (-2.9);

-- FLOAT 列插入 INT 字面量（应自动提升为浮点）
INSERT INTO t(f) VALUES (100);
INSERT INTO t(f) VALUES (200);

-- FLOAT 列插入 FLOAT 字面量
INSERT INTO t(f) VALUES (3.14159);
INSERT INTO t(f) VALUES (-2.5e-3);

-- VARCHAR 列
INSERT INTO t(v) VALUES ('hello');
INSERT INTO t(v) VALUES ('中文测试');
INSERT INTO t(v) VALUES ('special: ''quotes'' & symbols');

-- TEXT 列
INSERT INTO t(txt) VALUES ('long text content here...');

-- 读取所有
SELECT * FROM t;

-- 浮点比较
SELECT * FROM t WHERE f > 100.0;
SELECT * FROM t WHERE f = 3.14159;
SELECT * FROM t WHERE i > 0 AND f < 1000;

-- 类型混合运算
SELECT i, f, i + f AS sum, f * 2 AS doubled FROM t WHERE f IS NOT NULL;

exit;
