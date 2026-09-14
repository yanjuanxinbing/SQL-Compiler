-- ============================================================
-- 55_query: 查询 / 表达式扩展（Category 4）
-- ============================================================
-- 涵盖：
--   - LATERAL derived-table join  (cross-apply 语义)
--   - VALUES 行构造器作为 FROM 派生表
--   - FETCH FIRST n [ROW|ROWS] [ONLY|WITH TIES]
--   - OFFSET n [ROW|ROWS]  标准形式
--   - FOR UPDATE / FOR SHARE / FOR NO KEY UPDATE / FOR KEY SHARE
--   - 既有 LIMIT n OFFSET m 不回归
--
-- 备注：FOR UPDATE / FOR SHARE 在本单写引擎下是 parse-only hint；
-- 锁语义本身不强制执行（catalog 中记录，planner 在 PlanSelect 头部说明）。
-- ============================================================

-- ---------- 1. LATERAL derived-table join ----------
CREATE TABLE t1 (id INT, val INT);
INSERT INTO t1 VALUES (1, 10), (2, 20), (3, 30);
-- 对每条外层 t1 行，求满足 t2.val <= t1.val 的最大 val。
SELECT t1.id, sub.m AS max_le
FROM t1, LATERAL (SELECT MAX(val) AS m FROM t1 t2 WHERE t2.val <= t1.val) AS sub
ORDER BY t1.id;

-- ---------- 2. VALUES 行构造器作为 FROM 派生表 ----------
SELECT * FROM (VALUES (1, 'a'), (2, 'b'), (3, 'c')) AS t(id, name)
ORDER BY id;

-- ---------- 3. FETCH FIRST n ROWS ONLY ----------
CREATE TABLE fs (id INT);
INSERT INTO fs VALUES (1), (2), (3), (4), (5);
SELECT * FROM fs ORDER BY id FETCH FIRST 3 ROWS ONLY;

-- ---------- 4. OFFSET n ROWS + FETCH NEXT n ROWS ONLY ----------
SELECT * FROM fs ORDER BY id OFFSET 2 ROWS FETCH NEXT 2 ROWS ONLY;

-- ---------- 5. FOR UPDATE / FOR SHARE（parse-only hint）----------
SELECT * FROM fs WHERE id = 1 FOR UPDATE;
SELECT * FROM fs WHERE id = 1 FOR SHARE;

-- ---------- 6. 既有 LIMIT n OFFSET m 仍可用（回归检查）----------
SELECT * FROM fs ORDER BY id LIMIT 2 OFFSET 2;