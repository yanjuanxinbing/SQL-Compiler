-- 111_agg_over_join.sql
-- 测试目标：验证聚合与 JOIN 组合（GROUP BY 连接结果 / LEFT JOIN 空组 / HAVING 过滤）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. INNER JOIN + GROUP BY 客户名：统计订单数与总金额（含 NULL 金额行）
--   2. LEFT JOIN + GROUP BY：无订单客户也出现（SUM = NULL）
--   3. JOIN + GROUP BY + HAVING SUM(...) > 100
-- 预期结果：
--   - INNER：alice 2 单 150、bob 2 单 200（NULL 不计和）；carol 无单被排除
--   - LEFT：carol total=NULL
--   - HAVING：alice(150)、bob(200) 保留
-- 后置处理：DROP 测试表

CREATE TABLE c1(id INT PRIMARY KEY, cname VARCHAR);
CREATE TABLE o1(id INT PRIMARY KEY, cid INT, amount INT);

INSERT INTO c1 VALUES (1, 'alice'), (2, 'bob'), (3, 'carol');
INSERT INTO o1 VALUES (1, 1, 100), (2, 1, 50), (3, 2, 200), (4, 2, NULL);

-- 1) INNER JOIN 聚合
SELECT c.cname, COUNT(*) AS orders, SUM(o.amount) AS total
FROM c1 c JOIN o1 o ON c.id = o.cid
GROUP BY c.cname ORDER BY c.cname;

-- 2) LEFT JOIN 聚合（无订单客户保留）
SELECT c.cname, SUM(o.amount) AS total
FROM c1 c LEFT JOIN o1 o ON c.id = o.cid
GROUP BY c.cname ORDER BY c.cname;

-- 3) HAVING 过滤聚合结果
SELECT c.cname, SUM(o.amount) AS total
FROM c1 c JOIN o1 o ON c.id = o.cid
GROUP BY c.cname HAVING SUM(o.amount) > 100 ORDER BY c.cname;

-- 后置处理
DROP TABLE o1;
DROP TABLE c1;

exit;
