-- 120_insert_select_filtered.sql
-- 测试目标：验证 INSERT ... SELECT 的 WHERE 过滤与计算表达式源
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. INSERT ... SELECT ... WHERE b >= 20：行过滤后写入
--   2. INSERT ... SELECT 带计算表达式（a + 100, b * 2）与 WHERE
-- 预期结果（src: (1,10),(2,20),(3,30)）：
--   - 第一次：写入 (2,20),(3,30)
--   - 第二次：写入 (101, 20)
-- 后置处理：DROP 测试表

CREATE TABLE src(a INT PRIMARY KEY, b INT);
CREATE TABLE dst(x INT PRIMARY KEY, y INT);

INSERT INTO src VALUES (1, 10), (2, 20), (3, 30);

-- 1) WHERE 过滤
INSERT INTO dst SELECT a, b FROM src WHERE b >= 20;
SELECT x, y FROM dst ORDER BY x;

-- 2) 计算表达式源
INSERT INTO dst SELECT a + 100, b * 2 FROM src WHERE a = 1;
SELECT x, y FROM dst ORDER BY x;

-- 后置处理
DROP TABLE dst;
DROP TABLE src;

exit;
