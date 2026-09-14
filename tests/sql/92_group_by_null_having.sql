-- 92_group_by_null_having.sql
-- 测试目标：验证 GROUP BY 分组语义（NULL 归组、HAVING 过滤、空集分组）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. GROUP BY 单列：NULL 值应归为一个独立分组
--   2. GROUP BY + HAVING：对分组结果过滤（含 NULL 分组参与 HAVING）
--   3. GROUP BY + ORDER BY 聚合值排序
--   4. WHERE 过滤后空集 + GROUP BY → 0 行
--   5. 空集 GROUP BY + HAVING → 0 行（HAVING 不凭空造组）
-- 预期结果：
--   - dept 分组：a=2 行、NULL=2 行、b=1 行（两个 NULL 归同组）
--   - HAVING COUNT(*) >= 2 → a、NULL 两个分组
--   - SUM(sal) DESC → NULL组70、b组50、a组30
--   - WHERE 无匹配 → 0 行；HAVING 不改变空结果
-- 后置处理：DROP 测试表

CREATE TABLE g1(id INT, dept VARCHAR, sal INT);

INSERT INTO g1 VALUES
    (1, 'a', 10),
    (2, 'a', 20),
    (3, NULL, 30),
    (4, NULL, 40),
    (5, 'b', 50);

-- 1) NULL 归为一个独立分组
SELECT dept, COUNT(*) AS c FROM g1 GROUP BY dept;

-- 2) HAVING 过滤分组（NULL 分组同样参与）
SELECT dept, COUNT(*) AS c FROM g1 GROUP BY dept HAVING COUNT(*) >= 2;

-- 3) GROUP BY + ORDER BY 聚合列（DESC）
SELECT dept, SUM(sal) AS s FROM g1 GROUP BY dept ORDER BY s DESC;

-- 4) WHERE 后空集 + GROUP BY → 0 行
SELECT dept, COUNT(*) AS c FROM g1 WHERE id > 100 GROUP BY dept;

-- 5) 空集 GROUP BY + HAVING → 仍 0 行
SELECT dept, COUNT(*) AS c FROM g1 WHERE id > 100 GROUP BY dept HAVING COUNT(*) > 0;

-- 后置处理
DROP TABLE g1;

exit;
