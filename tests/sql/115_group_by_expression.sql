-- 109_group_by_expression.sql
-- 测试目标：验证 GROUP BY 按表达式分组（算术 / 字符串函数 / CASE）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. GROUP BY 算术表达式（id % 2）
--   2. GROUP BY 字符串函数（SUBSTR(name, 1, 1) 首字母）
--   3. GROUP BY CASE 表达式（按分数段分桶）
-- 预期结果（ge: alice/bob/carl/dan/eve，score 85/92/78/91/60）：
--   - parity：偶数 id 2 个、奇数 id 3 个
--   - initial：5 个首字母各 1 个
--   - band：high(>=90)=2（bob/dan）、low=3
-- 后置处理：DROP 测试表

CREATE TABLE ge(id INT PRIMARY KEY, name VARCHAR, score INT);

INSERT INTO ge VALUES
    (1, 'alice', 85),
    (2, 'bob', 92),
    (3, 'carl', 78),
    (4, 'dan', 91),
    (5, 'eve', 60);

-- 1) 算术表达式分组
SELECT id % 2 AS parity, COUNT(*) AS c FROM ge GROUP BY id % 2 ORDER BY parity;

-- 2) 函数表达式分组
SELECT SUBSTR(name, 1, 1) AS initial, COUNT(*) AS c
FROM ge GROUP BY SUBSTR(name, 1, 1) ORDER BY initial;

-- 3) CASE 表达式分组
SELECT CASE WHEN score >= 90 THEN 'high' ELSE 'low' END AS band, COUNT(*) AS c
FROM ge GROUP BY CASE WHEN score >= 90 THEN 'high' ELSE 'low' END ORDER BY band;

-- 后置处理
DROP TABLE ge;

exit;
