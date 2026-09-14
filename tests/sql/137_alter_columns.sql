-- 131_alter_columns.sql
-- 测试目标：验证 ALTER TABLE 的列级操作（DROP COLUMN / RENAME COLUMN）数据保持
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. ALTER TABLE ... DROP COLUMN drop_me：列删除，其余列数据保留
--   2. ALTER TABLE ... RENAME COLUMN keep_me TO renamed：改名，数据保留
--   3. 改名后按新列名查询、按旧列名查询（应报错）
-- 预期结果：
--   - DROP 后 SELECT * → (id, keep_me) = (1, 10)
--   - RENAME 后 SELECT * → (id, renamed) = (1, 10)
--   - 按旧列名查询 → 报错（stderr），不影响退出码
-- 后置处理：DROP 测试表

CREATE TABLE alt(id INT PRIMARY KEY, keep_me INT, drop_me INT);

INSERT INTO alt VALUES (1, 10, 99);

-- 1) 删除列
ALTER TABLE alt DROP COLUMN drop_me;
SELECT * FROM alt;

-- 2) 重命名列
ALTER TABLE alt RENAME COLUMN keep_me TO renamed;
SELECT * FROM alt;

-- 3) 新列名可查询 / 旧列名报错
SELECT id, renamed FROM alt WHERE renamed = 10;
SELECT id FROM alt WHERE keep_me = 10;

-- 后置处理
DROP TABLE alt;

exit;
