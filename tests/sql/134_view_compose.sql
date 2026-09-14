-- 128_view_compose.sql
-- 测试目标：验证视图组合能力（带 WHERE 的视图 / 视图上建视图 / 视图 JOIN 表）
--
-- BUG-20 回归：视图作为 JOIN 一侧时被当作真实表扫描（表不存在 → 静默 0 行）。
-- 修复后 JOIN 位置的视图展开为「派生表占位 + 视图查询子计划」。
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. CREATE VIEW 带 WHERE → 查询过滤生效
--   2. 在视图上 WHERE 过滤
--   3. 视图上再建视图（两层组合）
--   4. 视图 JOIN 表（视图在左侧）
--   5. 视图 JOIN 表 + GROUP BY
--   6. 表 JOIN 视图（视图在右侧）+ 聚合视图列
--   7. 视图 JOIN 表 + WHERE
-- 预期结果（vt: x=100, y=200, x=50, z=300；v_big=amt>60 → 1,2,4）：
--   - v_big 3 行；cat='x' → id=1
--   - v_x（v_big 且 cat='x'）→ (1, 100)
--   - 视图 JOIN → (1,1),(2,2),(4,4)
--   - GROUP BY cat → x:1, y:1, z:1；SUM(v_big.amt) → 100/200/300
--   - WHERE vt.cat='x' → id=1
-- 后置处理：DROP 视图与测试表

CREATE TABLE vt(id INT PRIMARY KEY, cat VARCHAR, amt INT);

INSERT INTO vt VALUES (1, 'x', 100), (2, 'y', 200), (3, 'x', 50), (4, 'z', 300);

-- 1) 带 WHERE 的视图
CREATE VIEW v_big AS SELECT id, cat, amt FROM vt WHERE amt > 60;
SELECT * FROM v_big ORDER BY id;

-- 2) 查询视图时追加 WHERE
SELECT id FROM v_big WHERE cat = 'x' ORDER BY id;

-- 3) 视图上建视图
CREATE VIEW v_x AS SELECT id, amt FROM v_big WHERE cat = 'x';
SELECT * FROM v_x ORDER BY id;

-- 4) 视图 JOIN 表（视图在左）
SELECT v_big.id, vt.id FROM v_big JOIN vt ON v_big.id = vt.id ORDER BY v_big.id;

-- 5) 视图 JOIN 表 + GROUP BY
SELECT vt.cat, COUNT(*) AS c
FROM v_big JOIN vt ON v_big.id = vt.id
GROUP BY vt.cat ORDER BY vt.cat;

-- 6) 表 JOIN 视图（视图在右）+ 聚合视图列
SELECT vt.cat, SUM(v_big.amt) AS s
FROM vt JOIN v_big ON v_big.id = vt.id
GROUP BY vt.cat ORDER BY vt.cat;

-- 7) 视图 JOIN 表 + WHERE
SELECT v_big.id FROM v_big JOIN vt ON v_big.id = vt.id WHERE vt.cat = 'x';

-- 后置处理
DROP VIEW v_x;
DROP VIEW v_big;
DROP TABLE vt;

exit;
