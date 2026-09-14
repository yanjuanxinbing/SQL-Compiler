-- 99_having_scalar_agg.sql
-- 测试目标：验证 HAVING 无 GROUP BY 时的标量聚合过滤语义
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. HAVING COUNT(*) > 1：整表作为单组，条件满足 → 返回 1 行
--   2. HAVING COUNT(*) > 99：条件不满足 → 0 行
--   3. WHERE 空集 + HAVING SUM(v) IS NULL：空集 SUM = NULL，IS NULL 成立 → 返回 NULL 行
-- 预期结果：
--   - COUNT(*) = 3 > 1 → 1 行（c=3）
--   - 3 > 99 不成立 → 0 行
--   - 空集 SUM = NULL → 1 行（s=NULL）
-- 后置处理：DROP 测试表

CREATE TABLE ob(id INT PRIMARY KEY, v INT);

INSERT INTO ob VALUES (1, 30), (2, 10), (3, 20);

-- 1) 无 GROUP BY 的 HAVING：整表单组，条件满足
SELECT COUNT(*) AS c FROM ob HAVING COUNT(*) > 1;

-- 2) 条件不满足 → 0 行
SELECT COUNT(*) AS c FROM ob HAVING COUNT(*) > 99;

-- 3) 空集标量聚合 + HAVING IS NULL
SELECT SUM(v) AS s FROM ob WHERE id > 100 HAVING SUM(v) IS NULL;

-- 后置处理
DROP TABLE ob;

exit;
