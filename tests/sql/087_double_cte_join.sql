-- 81_double_cte_join.sql
-- BUG 修复：双 CTE JOIN 的 cmap 错位
-- --------------------------------------------------------------------------
-- 背景：
--   ProjectExecutor 输出元组形状是 [select_values ++ underlying_tuple]。
--   SemanticAnalyzer 把 CTE 注册成"只有 select_list 列"的虚拟表，
--   BuildCombinedColumnIndexMap 据此累加 CTE 之间的 offset。
--   物化前若不裁剪，CTE 行宽度 = select_count + base_cols，
--   两个 CTE join 时 b.x 会被 cmap 指向 a.underlying_x 而不是真正的 b.x 列，
--   导致：
--     1) ON 条件失效 (Cartesian 行为，a.x = b.x 退化为 a.x = a.underlying_x 恒真变体)
--     2) SELECT b.<非首列> 拿到 a.<同名列> 的值
-- 修复：在 CteDefineExecutor 物化时按 select_list 宽度裁剪元组。
-- --------------------------------------------------------------------------

CREATE TABLE T(x INTEGER);
INSERT INTO T VALUES (1),(2),(3);

-- Bug 1：双 CTE self-equi-join 应该输出 3 行（笛卡尔积应为 9 行）
WITH a AS (SELECT x FROM T), b AS (SELECT x FROM T)
SELECT a.x, b.x FROM a JOIN b ON a.x = b.x;

-- Bug 2：CTE JOIN 涉及非首列时，列对齐应正确
CREATE TABLE users(id INTEGER, name VARCHAR(32));
CREATE TABLE orders(user_id INTEGER, amount INTEGER);
INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol'), (4, 'Dave');
INSERT INTO orders VALUES (1, 50), (2, 200), (3, 30), (4, 999), (1, 75);
WITH u AS (SELECT id, name FROM users),
     o AS (SELECT user_id, amount FROM orders)
SELECT u.name, o.amount
FROM u JOIN o ON u.id = o.user_id
WHERE o.amount >= 100;

-- 单 CTE 引用：保持原行为
WITH one AS (SELECT id, name FROM users)
SELECT one.name FROM one WHERE one.id > 1;

-- CTE JOIN CTE 多列：name / id 都要对齐
WITH u AS (SELECT id, name FROM users),
     o AS (SELECT user_id, amount FROM orders)
SELECT u.name, o.amount, u.id
FROM u JOIN o ON u.id = o.user_id;

exit;