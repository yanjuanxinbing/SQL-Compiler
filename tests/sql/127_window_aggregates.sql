-- 127_window_aggregates.sql
-- 测试目标：验证窗口聚合函数（SUM OVER PARTITION / 累计和 / 分区 ROW_NUMBER）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. SUM(v) OVER (PARTITION BY grp)：分区聚合（每行输出组总和）
--   2. SUM(v) OVER (ORDER BY id)：按 id 累计求和（running total）
--   3. ROW_NUMBER() OVER (PARTITION BY grp ORDER BY v DESC)：分区排名
-- 预期结果（hg: a=10,30；b=5,15）：
--   - 分区聚合：a 行 grp_sum=40、b 行 grp_sum=20
--   - 累计：10 / 40 / 45 / 60
--   - 分区排名（v DESC）：id2(a,30)=1、id1(a,10)=2、id4(b,15)=1、id3(b,5)=2
-- 后置处理：DROP 测试表

CREATE TABLE hg(id INT PRIMARY KEY, grp VARCHAR, v INT);

INSERT INTO hg VALUES (1, 'a', 10), (2, 'a', 30), (3, 'b', 5), (4, 'b', 15);

-- 1) 分区聚合
SELECT id, grp, v, SUM(v) OVER (PARTITION BY grp) AS grp_sum
FROM hg ORDER BY id;

-- 2) 累计求和
SELECT id, v, SUM(v) OVER (ORDER BY id) AS running
FROM hg ORDER BY id;

-- 3) 分区 ROW_NUMBER
SELECT id, ROW_NUMBER() OVER (PARTITION BY grp ORDER BY v DESC) AS rn
FROM hg ORDER BY id;

-- 后置处理
DROP TABLE hg;

exit;
