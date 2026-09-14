-- 132_txn_ddl_rollback.sql
-- 测试目标：验证事务中 DDL 与 DML 混合时的 ROLLBACK 边界
--         （实测行为：DDL 在提交时隐式生效（MySQL 风格隐式提交），
--           ROLLBACK 撤销事务内的 DML —— 表保留但为空）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. BEGIN → CREATE TABLE txnt → INSERT 1 行 → ROLLBACK
--   2. 查询该表：表存在（DDL 已隐式生效），行数为 0（INSERT 已回滚）
--   3. 事务外正常 INSERT 后提交语义不受影响
-- 预期结果：
--   - ROLLBACK 后 COUNT(*) = 0（表存在、无行）
--   - 再次 INSERT（自动提交）→ COUNT = 1
-- 后置处理：DROP 测试表

BEGIN;
CREATE TABLE txnt(id INT PRIMARY KEY);
INSERT INTO txnt VALUES (1);
ROLLBACK;

-- 1) 表保留（DDL 隐式提交），数据回滚
SELECT COUNT(*) AS rows_after_rollback FROM txnt;

-- 2) 自动提交路径正常
INSERT INTO txnt VALUES (1);
SELECT COUNT(*) AS rows_after_insert FROM txnt;

-- 后置处理
DROP TABLE txnt;

exit;
