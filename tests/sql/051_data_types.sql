-- 52_data_types.sql
-- 52_data_types: 新增数据类型 + NOT NULL / UNIQUE / AUTO_INCREMENT 执行期语义。
--   覆盖：BOOLEAN / BOOL / CHAR(n) / TEXT / DECIMAL(p,s) / DOUBLE / REAL /
--   SMALLINT / TINYINT / TIME / JSON / UUID，列级 UNIQUE，列级 NOT NULL，
--   主键 AUTO_INCREMENT 自动编号，以及 DECIMAL 精确文本持久化。

-- ====================================================================
-- 1) 全类型建表：AUTO_INCREMENT / 11 种新类型混排
-- ====================================================================
CREATE TABLE t (
    id INT PRIMARY KEY AUTO_INCREMENT,
    b  BOOLEAN NOT NULL,
    c  CHAR(5),
    txt TEXT,
    dec DECIMAL(10, 2),
    d  DOUBLE,
    r  REAL,
    s  SMALLINT,
    ti TINYINT,
    ts TIME,
    j  JSON,
    u  UUID
);

-- 全部字段显式提供：AUTO_INCREMENT 主键同时显式赋值，行为同普通 INSERT。
INSERT INTO t(id, b, c, txt, dec, d, r, s, ti, ts, j, u) VALUES
    (1, TRUE, 'abc', 'long text', 123.45, 3.14, 2.71, 1000, 127,
     '12:30:45', '{"k":1}', '550e8400-e29b-41d4-a716-446655440000');

-- 读取所有列；SELECT * 必须按表定义顺序返回。
SELECT * FROM t WHERE id = 1;

-- 表达式比较：b = TRUE / dec = 123.45 / d > 3.0
--   - b 列存为 INTEGER(1) 与 BOOLEAN 字面量比较 → TRUE
--   - dec 列存为 VARCHAR "123.45" 与 FLOAT 123.45 比较 → TRUE
--   - d 列存为 FLOAT 3.14 与 FLOAT 3.0 比较 → TRUE
SELECT b = TRUE, dec = 123.45, d > 3.0 FROM t WHERE id = 1;

-- ====================================================================
-- 2) NOT NULL 列级约束
-- ====================================================================
-- 2a) BOOLEAN NOT NULL 列显式 NULL → 拒绝
INSERT INTO t(id, b) VALUES (2, NULL);

-- 2b) BOOLEAN NOT NULL 列显式 FALSE → 接受（FALSE != NULL）
INSERT INTO t(id, b) VALUES (3, FALSE);
SELECT id, b FROM t WHERE id = 3;
SELECT COUNT(*) FROM t;

-- 2c) 整型列省略 (即未提供且无 DEFAULT) → 等价显式 NULL；若声明了 NOT NULL 则拒绝。
CREATE TABLE nn (id INT, name VARCHAR NOT NULL);
SELECT id FROM nn WHERE 1 = 0;
INSERT INTO nn(id) VALUES (1);

-- ====================================================================
-- 3) UNIQUE 列级 / 表级约束
-- ====================================================================
CREATE TABLE uq (id INT, email VARCHAR UNIQUE);
INSERT INTO uq VALUES (1, 'a@x');
INSERT INTO uq VALUES (2, 'a@x');

CREATE TABLE uq2 (id INT, a VARCHAR, b VARCHAR, UNIQUE(a, b));
INSERT INTO uq2 VALUES (1, 'x', 'y');
INSERT INTO uq2 VALUES (2, 'x', 'y');

-- ====================================================================
-- 4) AUTO_INCREMENT 自动编号
-- ====================================================================
-- 4a) 显式 NULL → 下一个 id。本表当前最大 id=3 → 新行 id=4。
INSERT INTO t(id, b) VALUES (NULL, TRUE);
SELECT id FROM t WHERE b = TRUE ORDER BY id;

-- 4b) 显式 0 → 同样触发自增
INSERT INTO t(id, b) VALUES (0, TRUE);
SELECT id FROM t WHERE b = TRUE ORDER BY id;

-- 4c) SERIAL 简写 = INT PRIMARY KEY AUTO_INCREMENT NOT NULL
CREATE TABLE seq (id SERIAL, name VARCHAR);
INSERT INTO seq(name) VALUES ('a');
INSERT INTO seq(name) VALUES ('b');
INSERT INTO seq(name) VALUES ('c');
SELECT * FROM seq ORDER BY id;

-- ====================================================================
-- 5) DECIMAL 精度：99999999.99 不应受 IEEE-754 噪声污染
-- ====================================================================
INSERT INTO t(id, b, dec) VALUES (100, TRUE, 99999999.99);
SELECT dec FROM t WHERE id = 100;

-- 普通十进制比较：10000.50 / 0.50 之类
INSERT INTO t(id, b, dec) VALUES (101, TRUE, 10000.5);
SELECT dec FROM t WHERE id = 101;

-- ====================================================================
-- 6) 列级 CHECK 与 DEFAULT 在新类型列上仍可用
-- ====================================================================
-- DECIMAL 列挂 CHECK (dec >= 0) + DEFAULT '0.00'，验证非空路径 + 约束路径。
CREATE TABLE money (
    id INT PRIMARY KEY,
    amt DECIMAL(12, 2) NOT NULL DEFAULT 0.00 CHECK (amt >= 0)
);
INSERT INTO money(id, amt) VALUES (1, 100.50);
INSERT INTO money(id) VALUES (2);
SELECT * FROM money ORDER BY id;
SELECT COUNT(*) FROM money;
SELECT amt FROM money WHERE 1 = 0;
INSERT INTO money(id, amt) VALUES (3, -1);

exit;