-- 77_self_join.sql
-- 测试目标：验证自连接查询的语义正确性
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 建立员工-经理层次结构表
--   2. INNER JOIN 自连接：查询员工及其直接上级
--   3. LEFT JOIN 自连接：顶级经理（mgr_id IS NULL）也出现在结果中
--   4. 多层层次：验证 3 层组织架构
--   5. 自连接 + WHERE 过滤
-- 预期结果：
--   - INNER JOIN 返回 3 行（顶级 boss 无上级被排除）
--   - LEFT JOIN 返回 4 行（boss 的 mgr_name 为 NULL）
--   - WHERE e.name LIKE 'a%' 过滤后 INNER JOIN 返回 1 行
-- 后置处理：DROP 测试表

CREATE TABLE emp(
    id INT PRIMARY KEY,
    name VARCHAR,
    mgr_id INT
);

-- 层次：boss(1) ← alice(2), bob(3) ← carl(4)
INSERT INTO emp VALUES (1, 'boss', NULL), (2, 'alice', 1), (3, 'bob', 1), (4, 'carl', 2);

-- INNER JOIN：员工 + 直接上级
SELECT e.name AS emp_name, m.name AS mgr_name
FROM emp e JOIN emp m ON e.mgr_id = m.id
ORDER BY e.id;

-- LEFT JOIN：顶级经理也在结果中（mgr_name 为 NULL）
SELECT e.name AS emp_name, m.name AS mgr_name
FROM emp e LEFT JOIN emp m ON e.mgr_id = m.id
ORDER BY e.id;

-- 自连接 + WHERE 过滤
SELECT e.name AS emp_name, m.name AS mgr_name
FROM emp e JOIN emp m ON e.mgr_id = m.id
WHERE e.name LIKE 'a%'
ORDER BY e.id;

-- 验证全表行数
SELECT COUNT(*) AS total FROM emp;
SELECT COUNT(mgr_id) AS has_mgr FROM emp;

-- 后置处理
DROP TABLE emp;

exit;
