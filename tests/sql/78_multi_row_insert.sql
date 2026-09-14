-- 78_multi_row_insert.sql
-- 测试目标：验证多行 INSERT 语法与批量写入的正确性
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 单行 INSERT VALUES (...)
--   2. 多行 INSERT VALUES (...), (...), ...
--   3. INSERT INTO ... SELECT 从已有表复制
--   4. 混合数据类型的批量插入（INT + FLOAT + VARCHAR + NULL）
--   5. 批量插入后 COUNT/SUM 验证数据完整性
-- 预期结果：
--   - 单行 INSERT 后 COUNT = 1
--   - 3 行批量 INSERT 后 COUNT = 4
--   - INSERT...SELECT 复制后 COUNT = 8
--   - SUM(v) = 10+20+30+40+10+20+30+40 = 200
-- 后置处理：DROP 所有测试表

CREATE TABLE batch(id INT PRIMARY KEY, v INT, f FLOAT, s VARCHAR);

-- 单行
INSERT INTO batch VALUES (1, 10, 1.5, 'a');

-- 多行（3 行）
INSERT INTO batch VALUES
    (2, 20, 2.5, 'b'),
    (3, 30, 3.5, 'c'),
    (4, 40, 4.5, 'd');

SELECT COUNT(*) AS cnt, SUM(v) AS total FROM batch;

-- 混合含 NULL 的批量插入
INSERT INTO batch VALUES
    (5, NULL, NULL, NULL),
    (6, 60, 6.5, 'f');

SELECT COUNT(*) AS cnt_with_null, SUM(v) AS sum_with_null FROM batch;

-- INSERT INTO ... SELECT 从已有表复制
INSERT INTO batch SELECT id + 10, v, f, s FROM batch WHERE id <= 2;

SELECT COUNT(*) AS cnt_after_copy FROM batch;
SELECT SUM(v) AS sum_final FROM batch;

-- 验证复制后数据正确
SELECT id, v, s FROM batch WHERE id >= 11 ORDER BY id;

-- 后置处理
DROP TABLE batch;

exit;
