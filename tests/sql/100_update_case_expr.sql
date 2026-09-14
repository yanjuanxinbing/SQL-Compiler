-- 100_update_case_expr.sql
-- 测试目标：验证 UPDATE ... SET 使用表达式、CASE 分支、函数求值的正确性
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. SET v = v * 2 + 1：基于列自身值的算术表达式
--   2. SET tag = CASE WHEN ...：按行条件批量重写列值
--   3. SET v = ABS(v)：NULL 参与函数 → 结果保持 NULL
--   4. SET tag = UPPER(tag)：字符串函数作用于列
-- 预期结果：
--   - id=1: v 10 → 21
--   - CASE 重写后：v>0→pos、v<0→neg、v=NULL→zero（ELSE 兜底）
--   - ABS(NULL) = NULL，v 保持 NULL；UPPER('zero') = 'ZERO'
-- 后置处理：DROP 测试表

CREATE TABLE u1(id INT PRIMARY KEY, v INT, tag VARCHAR);

INSERT INTO u1 VALUES (1, 10, 'a'), (2, -5, 'b'), (3, NULL, 'c');

-- 1) 基于列自身值的表达式
UPDATE u1 SET v = v * 2 + 1 WHERE id = 1;
SELECT id, v FROM u1 WHERE id = 1;

-- 2) CASE 表达式批量重写（NULL 落入 ELSE）
UPDATE u1 SET tag = CASE WHEN v > 0 THEN 'pos'
                         WHEN v < 0 THEN 'neg'
                         ELSE 'zero' END;
SELECT id, v, tag FROM u1 ORDER BY id;

-- 3) + 4) 函数作用于列；ABS(NULL) 保持 NULL
UPDATE u1 SET v = ABS(v), tag = UPPER(tag) WHERE id = 3;
SELECT id, v, tag FROM u1 WHERE id = 3;

-- 后置处理
DROP TABLE u1;

exit;
