-- 21_ddl_variants.sql
-- DDL 变体：CREATE IF NOT EXISTS / 重新创建 / 多列 PK / DROP 后重建

-- 简单表
CREATE TABLE tmp1(id INT, val INT);

-- IF NOT EXISTS：第一次应成功
CREATE TABLE IF NOT EXISTS tmp1(id INT, val INT);

-- IF NOT EXISTS：第二次不应报错
CREATE TABLE IF NOT EXISTS tmp2(id INT, val INT);
CREATE TABLE IF NOT EXISTS tmp2(id INT, val INT);

-- 单列 PK
CREATE TABLE t_pk1(id INT PRIMARY KEY, note VARCHAR);

-- 多列 PK
CREATE TABLE t_pk2(
    a INT,
    b INT,
    val VARCHAR,
    PRIMARY KEY(a, b)
);

-- 多种类型混用
CREATE TABLE t_mixed(
    i INT,
    bi BIGINT,
    f FLOAT,
    d DOUBLE,
    v VARCHAR(255),
    t TEXT
);

-- 大量列
CREATE TABLE t_wide(
    c1 INT, c2 INT, c3 INT, c4 INT, c5 INT,
    c6 INT, c7 INT, c8 INT, c9 INT, c10 INT
);

-- 插入数据
INSERT INTO t_pk1 VALUES (1, 'first');
INSERT INTO t_pk2 VALUES (1, 2, 'one-two');
INSERT INTO t_pk2 VALUES (1, 3, 'one-three');
INSERT INTO t_mixed(i, bi, f, d, v, t) VALUES (1, 100, 1.5, 2.5, 'hi', 'long text');
INSERT INTO t_wide VALUES (1, 2, 3, 4, 5, 6, 7, 8, 9, 10);

SELECT * FROM t_pk1;
SELECT * FROM t_pk2;
SELECT * FROM t_mixed;
SELECT * FROM t_wide;

-- DROP 后重建
DROP TABLE t_pk1;
CREATE TABLE t_pk1(id INT, note VARCHAR);
INSERT INTO t_pk1 VALUES (1, 'after-drop');
SELECT * FROM t_pk1;

exit;