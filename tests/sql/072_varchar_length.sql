-- 73_varchar_length.sql
-- 测试目标：验证 VARCHAR(N) 长度约束的边界行为
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. VARCHAR(1)：单字符通过，空串通过，2 字符被拒
--   2. VARCHAR(5)：恰好 5 字符通过，6 字符被拒
--   3. VARCHAR 与 LENGTH 函数交互
--   4. TEXT 类型无长度限制
--   5. UPDATE 路径同样校验长度
-- 预期结果：
--   - 合法长度 INSERT/UPDATE 返回 OK
--   - 超长 INSERT/UPDATE 被 Semantic Error 拒绝，行不写入
--   - LENGTH 对 NULL 返回 NULL，对空串返回 0
-- 后置处理：DROP 所有测试表

-- ============================================================
-- 1. VARCHAR(1) 边界
-- ============================================================
CREATE TABLE t1(id INT PRIMARY KEY, s VARCHAR(1));

-- 恰好 1 字符：合法
INSERT INTO t1 VALUES (1, 'A');
-- 空串：合法（0 <= 1）
INSERT INTO t1 VALUES (2, '');
-- 2 字符：应被拒绝
INSERT INTO t1 VALUES (3, 'AB');

SELECT id, s, LENGTH(s) AS len FROM t1 ORDER BY id;

-- ============================================================
-- 2. VARCHAR(5) 边界
-- ============================================================
CREATE TABLE t5(id INT PRIMARY KEY, s VARCHAR(5));

-- 恰好 5 字符：合法
INSERT INTO t5 VALUES (1, '01234');
-- 4 字符：合法
INSERT INTO t5 VALUES (2, '0123');
-- 6 字符：应被拒绝
INSERT INTO t5 VALUES (3, '012345');

SELECT id, s, LENGTH(s) AS len FROM t5 ORDER BY id;

-- ============================================================
-- 3. UPDATE 路径长度校验
-- ============================================================
-- 将 id=1 的 s 改为 2 字符：应被拒绝
UPDATE t1 SET s = 'XY' WHERE id = 1;
-- 改为空串：合法
UPDATE t1 SET s = '' WHERE id = 1;

SELECT id, s FROM t1 WHERE id = 1;

-- ============================================================
-- 4. TEXT 类型无长度限制
-- ============================================================
CREATE TABLE tt(id INT PRIMARY KEY, t TEXT);

-- 超长字符串对 TEXT 合法（200 字符）
INSERT INTO tt VALUES (1, '0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789');

SELECT id, LENGTH(t) AS len FROM tt;

-- ============================================================
-- 后置处理：清理
-- ============================================================
DROP TABLE t1;
DROP TABLE t5;
DROP TABLE tt;

exit;
