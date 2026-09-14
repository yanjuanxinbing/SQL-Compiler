-- 98_order_by_alias_expr.sql
-- 测试目标：验证 ORDER BY 引用 select 别名、原始列、表达式及多键排序
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. ORDER BY select 别名（dbl = v * 2）
--   2. ORDER BY 原始表达式（v * 2）—— 与别名排序结果一致
--   3. ORDER BY 别名 DESC + 第二键 id ASC
--   4. ORDER BY 恒等表达式（v + 0）
-- 预期结果：
--   - ob: (1,30),(2,10),(3,20)；按 v*2 升序 → (2,20),(3,40),(1,60)
--   - dbl DESC, id ASC → (1,60),(3,40),(2,20)
--   - v+0 升序 → (2,10),(3,20),(1,30)
-- 后置处理：DROP 测试表

CREATE TABLE ob(id INT PRIMARY KEY, v INT);

INSERT INTO ob VALUES (1, 30), (2, 10), (3, 20);

-- 1) 按别名排序
SELECT id, v * 2 AS dbl FROM ob ORDER BY dbl;

-- 2) 按表达式排序（与别名等价）
SELECT id, v * 2 AS dbl FROM ob ORDER BY v * 2;

-- 3) 多键：别名 DESC + id ASC
SELECT id, v * 2 AS dbl FROM ob ORDER BY dbl DESC, id ASC;

-- 4) 恒等表达式排序
SELECT id, v FROM ob ORDER BY v + 0;

-- 后置处理
DROP TABLE ob;

exit;
