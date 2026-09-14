-- 76_in_between_null.sql
-- 测试目标：验证 IN / NOT IN / BETWEEN 与 NULL 的三值逻辑语义
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. IN 运算：单值、多值匹配
--   2. NOT IN：排除已有值
--   3. NOT IN 含 NULL 值数据 → SQL 标准：结果为空（unknown 传播）
--   4. BETWEEN 边界：包含两端（闭区间）
--   5. NOT BETWEEN
--   6. 反向 BETWEEN（low > high）→ 空结果
--   7. IS NULL / IS NOT NULL
-- 预期结果：
--   - IN (10, 30) → id 1, 4
--   - NOT IN (10) → id 2, 4
--   - NOT IN (10, NULL) → 空集（NULL 导致 unknown 传播）
--   - BETWEEN 10 AND 20 → id 1, 2
--   - NOT BETWEEN 10 AND 20 → id 4
--   - BETWEEN 20 AND 10 → 空集
--   - IS NULL → id 3
-- 后置处理：DROP 测试表

CREATE TABLE inb(id INT PRIMARY KEY, v INT);

INSERT INTO inb VALUES (1, 10), (2, 20), (3, NULL), (4, 30);

-- IN 单值
SELECT id FROM inb WHERE v IN (10);

-- IN 多值
SELECT id FROM inb WHERE v IN (10, 30);

-- NOT IN 单值
SELECT id FROM inb WHERE v NOT IN (10);

-- NOT IN 多值含 NULL → SQL 标准：unknown 传播 → 空结果
SELECT id FROM inb WHERE v NOT IN (10, NULL);

-- BETWEEN 闭区间
SELECT id FROM inb WHERE v BETWEEN 10 AND 20;

-- NOT BETWEEN
SELECT id FROM inb WHERE v NOT BETWEEN 10 AND 20;

-- 反向 BETWEEN（low > high）→ 空结果
SELECT id FROM inb WHERE v BETWEEN 20 AND 10;

-- IS NULL
SELECT id FROM inb WHERE v IS NULL;

-- IS NOT NULL
SELECT id FROM inb WHERE v IS NOT NULL ORDER BY id;

-- 后置处理
DROP TABLE inb;

exit;
