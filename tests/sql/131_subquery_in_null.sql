-- 125_subquery_in_null.sql
-- 测试目标：验证 IN / NOT IN (SELECT ...) 子查询源的 NULL 三值逻辑传播
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. IN (SELECT 含 NULL)：非 NULL 且命中的行保留
--   2. NOT IN (SELECT 含 NULL)：SQL 标准 —— 集合含 NULL 时对任何值
--      NOT IN 均为 UNKNOWN → 空结果
--   3. IN (SELECT 单值常量)
-- 预期结果（t1.v = 10/20/30/NULL；t2.ref = 10/NULL/40）：
--   - IN → id=1（10 命中 {10,NULL,40}）
--   - NOT IN → 0 行（NULL 传播）
--   - IN (SELECT 10) → id=1
-- 后置处理：DROP 测试表

CREATE TABLE t1(id INT PRIMARY KEY, v INT);
CREATE TABLE t2(id INT PRIMARY KEY, ref INT);

INSERT INTO t1 VALUES (1, 10), (2, 20), (3, 30), (4, NULL);
INSERT INTO t2 VALUES (1, 10), (2, NULL), (3, 40);

-- 1) IN 子查询（NULL 不匹配）
SELECT id FROM t1 WHERE v IN (SELECT ref FROM t2) ORDER BY id;

-- 2) NOT IN 子查询（NULL 传播 → 空集）
SELECT id FROM t1 WHERE v NOT IN (SELECT ref FROM t2) ORDER BY id;

-- 3) IN 单值子查询
SELECT id FROM t1 WHERE v IN (SELECT 10) ORDER BY id;

-- 后置处理
DROP TABLE t2;
DROP TABLE t1;

exit;
