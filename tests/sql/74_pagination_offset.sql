-- 74_pagination_offset.sql
-- 测试目标：验证 LIMIT ... OFFSET 分页语义
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 插入 12 行有序数据
--   2. LIMIT N OFFSET 0（第一页）
--   3. LIMIT N OFFSET M（中间页）
--   4. OFFSET 超出数据范围 → 空结果
--   5. LIMIT 0 → 空结果
--   6. LIMIT 超出总行数 → 返回所有剩余行
--   7. ORDER BY + LIMIT + OFFSET 组合
-- 预期结果：
--   - 第 1 页 (LIMIT 5 OFFSET 0)：id 1~5
--   - 第 2 页 (LIMIT 5 OFFSET 5)：id 6~10
--   - 第 3 页 (LIMIT 5 OFFSET 10)：id 11~12
--   - OFFSET 12 → 0 行
--   - LIMIT 100 OFFSET 0 → 12 行
--   - LIMIT 0 → 0 行
-- 后置处理：DROP 测试表

CREATE TABLE page(id INT, val INT);

INSERT INTO page VALUES (1, 10),(2, 20),(3, 30),(4, 40),(5, 50);
INSERT INTO page VALUES (6, 60),(7, 70),(8, 80),(9, 90),(10, 100);
INSERT INTO page VALUES (11, 110),(12, 120);

-- 第 1 页
SELECT id FROM page ORDER BY id LIMIT 5 OFFSET 0;

-- 第 2 页
SELECT id FROM page ORDER BY id LIMIT 5 OFFSET 5;

-- 第 3 页（不足 5 行）
SELECT id FROM page ORDER BY id LIMIT 5 OFFSET 10;

-- OFFSET 等于总行数 → 空结果
SELECT id FROM page ORDER BY id LIMIT 5 OFFSET 12;

-- OFFSET 超出总行数 → 空结果
SELECT id FROM page ORDER BY id LIMIT 5 OFFSET 100;

-- LIMIT 0 → 空结果
SELECT id FROM page ORDER BY id LIMIT 0;

-- LIMIT 超出总行数 → 返回所有行
SELECT id FROM page ORDER BY id LIMIT 100 OFFSET 0;

-- 逗号语法 LIMIT offset, count
SELECT id FROM page ORDER BY id LIMIT 0, 3;
SELECT id FROM page ORDER BY id LIMIT 9, 5;

-- 后置处理
DROP TABLE page;

exit;
