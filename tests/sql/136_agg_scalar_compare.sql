-- 130_agg_scalar_compare.sql
-- 测试目标：验证 WHERE 中与聚合标量子查询的比较（高于平均值等经典模式）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. WHERE v > (SELECT AVG(v) FROM hg)：高于组均值的行
--   2. WHERE v < (SELECT AVG(v) FROM hg)：低于均值的行
--   3. WHERE v = (SELECT MAX(v) FROM hg)：等于最大值
-- 预期结果（hg.v = 10/30/5/15，AVG=15，MAX=30）：
--   - v > 15 → id=2（30）
--   - v < 15 → id=1（10）、id=3（5）
--   - v = 30 → id=2
-- 后置处理：DROP 测试表

CREATE TABLE hg(id INT PRIMARY KEY, grp VARCHAR, v INT);

INSERT INTO hg VALUES (1, 'a', 10), (2, 'a', 30), (3, 'b', 5), (4, 'b', 15);

-- 1) 高于平均值
SELECT id FROM hg WHERE v > (SELECT AVG(v) FROM hg) ORDER BY id;

-- 2) 低于平均值
SELECT id FROM hg WHERE v < (SELECT AVG(v) FROM hg) ORDER BY id;

-- 3) 等于最大值
SELECT id FROM hg WHERE v = (SELECT MAX(v) FROM hg) ORDER BY id;

-- 后置处理
DROP TABLE hg;

exit;
