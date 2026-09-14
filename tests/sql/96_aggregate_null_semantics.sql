-- 96_aggregate_null_semantics.sql
-- 测试目标：验证聚合函数对 NULL 的标准 SQL 语义（部分 NULL / 全 NULL / 空集）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 部分 NULL 列：COUNT(*) 计全部行，COUNT(col) 跳过 NULL，
--      SUM/AVG/MIN/MAX 忽略 NULL
--   2. 全 NULL 列：COUNT(col)=0，SUM/AVG/MIN/MAX = NULL
--   3. 空集（WHERE 无匹配）：标量聚合返回一行（SUM/AVG = NULL）
--   4. AVG 只对非 NULL 值取平均
-- 预期结果：
--   - 表 ag（3 行，v = NULL,NULL,5）：c_star=3, c_v=1, s=5, a=5, mn=5, mx=5
--   - 表 ag2（2 行，v = NULL,NULL）：c_star=2, c_v=0, s=NULL, a=NULL, mn=NULL, mx=NULL
--   - 空集聚合：s=NULL, a=NULL（仍返回 1 行）
-- 后置处理：DROP 测试表

CREATE TABLE ag(id INT PRIMARY KEY, v INT);

INSERT INTO ag VALUES (1, NULL), (2, NULL), (3, 5);

-- 1) 部分 NULL：COUNT(*) 与 COUNT(col) 差异；其余聚合忽略 NULL
SELECT COUNT(*) AS c_star,
       COUNT(v) AS c_v,
       SUM(v)  AS s,
       AVG(v)  AS a,
       MIN(v)  AS mn,
       MAX(v)  AS mx
FROM ag;

-- AVG 只对非 NULL 取平均
SELECT AVG(v) AS avg_non_null_only FROM ag;

-- 2) 全 NULL 列
CREATE TABLE ag2(id INT PRIMARY KEY, v INT);

INSERT INTO ag2 VALUES (1, NULL), (2, NULL);

SELECT COUNT(*) AS c_star,
       COUNT(v) AS c_v,
       SUM(v)  AS s,
       AVG(v)  AS a,
       MIN(v)  AS mn,
       MAX(v)  AS mx
FROM ag2;

-- 3) 空集：标量聚合仍返回一行，SUM/AVG = NULL
SELECT SUM(v) AS s, AVG(v) AS a FROM ag2 WHERE id > 100;

-- 后置处理
DROP TABLE ag;
DROP TABLE ag2;

exit;
