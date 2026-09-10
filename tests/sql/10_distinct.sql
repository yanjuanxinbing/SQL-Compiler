-- 10_distinct.sql
-- DISTINCT 关键字：单列 / 多列 / 与聚合组合

CREATE TABLE t(id INT, dept VARCHAR, name VARCHAR, score FLOAT);

INSERT INTO t VALUES
    (1, 'IT',  'Alice',  88.5),
    (2, 'IT',  'Bob',    91.0),
    (3, 'HR',  'Charlie',76.5),
    (4, 'IT',  'David',  88.5),
    (5, 'HR',  'Eve',    76.5),
    (6, 'IT',  'Frank',  91.0),
    (7, 'HR',  'Grace',  85.0);

-- 普通查询（带重复）
SELECT dept FROM t;

-- 单列 DISTINCT
SELECT DISTINCT dept FROM t;

-- 多列 DISTINCT（视为组合唯一）
SELECT DISTINCT dept, score FROM t;

-- DISTINCT + ORDER BY
SELECT DISTINCT dept FROM t ORDER BY dept DESC;

-- COUNT(DISTINCT col)
SELECT COUNT(DISTINCT dept) AS dept_cnt FROM t;
SELECT COUNT(DISTINCT score) AS score_cnt FROM t;

-- DISTINCT 与 GROUP BY 同时使用
SELECT dept, COUNT(DISTINCT score) AS uniq_scores
FROM t
GROUP BY dept
ORDER BY dept;

exit;