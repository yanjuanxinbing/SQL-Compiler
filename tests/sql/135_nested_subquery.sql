-- 129_nested_subquery.sql
-- 测试目标：验证嵌套子查询（深度 2：子查询内再嵌子查询）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 标量子查询内嵌标量子查询（比较条件引用更内层聚合）
--   2. IN (SELECT ... IN (SELECT ...)) 双层 IN
-- 预期结果（t1.v = 10/20/30/NULL；t2.ref = 10/NULL/40）：
--   - 内层 MAX(v)=30 → ref<30 → {10} → MAX(ref)=10 → v=10 → id=1
--   - 双层 IN：ref ∈ v 值集 {10,20,30,NULL} → {10} → v IN {10} → id=1
-- 后置处理：DROP 测试表

CREATE TABLE t1(id INT PRIMARY KEY, v INT);
CREATE TABLE t2(id INT PRIMARY KEY, ref INT);

INSERT INTO t1 VALUES (1, 10), (2, 20), (3, 30), (4, NULL);
INSERT INTO t2 VALUES (1, 10), (2, NULL), (3, 40);

-- 1) 标量子查询嵌套
SELECT id FROM t1
WHERE v = (SELECT MAX(ref) FROM t2 WHERE ref < (SELECT MAX(v) FROM t1));

-- 2) 双层 IN
SELECT id FROM t1
WHERE v IN (SELECT ref FROM t2 WHERE ref IN (SELECT v FROM t1)) ORDER BY id;

-- 后置处理
DROP TABLE t2;
DROP TABLE t1;

exit;
