-- 121_insert_column_subset.sql
-- 测试目标：验证 INSERT 只列出部分列时，省略列填充 NULL
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. INSERT INTO ic(id) VALUES：a、b 均省略 → NULL
--   2. INSERT INTO ic(id, a) VALUES：b 省略 → NULL
-- 预期结果：
--   - (1, NULL, NULL)
--   - (2, 5, NULL)
-- 后置处理：DROP 测试表

CREATE TABLE ic(id INT PRIMARY KEY, a INT, b VARCHAR);

INSERT INTO ic(id) VALUES (1);
INSERT INTO ic(id, a) VALUES (2, 5);

SELECT id, a, b FROM ic ORDER BY id;

-- 后置处理
DROP TABLE ic;

exit;
