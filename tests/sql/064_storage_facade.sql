-- ============================================================
-- 65_storage_facade.sql
-- Spec 2.3 "接口设计与数据库集成"：统一的存储访问接口测试。
--
-- Database 构造期把 BufferPoolManager + DiskManager 包装成 StorageAccess
-- 单例（Database::Storage() / ctx.GetStorage()）。本测试不直接观测门面，但
-- 所有 DML / DDL / SELECT 都走 TableHeap → BPM 这条路径，门面在它们背后
-- 默默转发了 GetPage / NewPage / UnpinPage / FlushPage。
--
-- 这里覆盖一个完整 CRUD 生命周期，并触发聚合 / join / 子查询，验证门面
-- 后端没有引入回归：
--   1) CREATE TABLE（含 NewPage 路径）
--   2) INSERT（NewPage + UnpinPage）
--   3) SELECT（GetPage 反复 pin/unpin）
--   4) UPDATE（GetPage + 标脏 + UnpinPage）
--   5) DELETE（GetPage + 标脏 + UnpinPage）
--   6) DROP TABLE
-- ============================================================

CREATE TABLE sfacade (id INT PRIMARY KEY, name VARCHAR, score INT);

INSERT INTO sfacade VALUES
    (1, 'alice', 90),
    (2, 'bob',   75),
    (3, 'carol', 88),
    (4, 'dave',  60);

SELECT COUNT(*) AS n_rows FROM sfacade;
SELECT SUM(score) AS total_score FROM sfacade;
SELECT id, name FROM sfacade WHERE score >= 80 ORDER BY id;

UPDATE sfacade SET score = score + 5 WHERE id IN (1, 3);
SELECT id, name, score FROM sfacade ORDER BY id;

DELETE FROM sfacade WHERE id = 2;
SELECT COUNT(*) AS n_rows_after_delete FROM sfacade;

DROP TABLE sfacade;

exit;