-- 63_prune_columns.sql
-- 列裁剪验证：SELECT 只取若干列，optimizer 应把 SeqScanNode.read_columns 裁剪为
-- 仅含 select_list 引用的列；执行结果与未裁剪时一致。
--
-- 表 schema 有 5 列：id / name / dept / age / salary。
-- 验证场景：
--   1) SELECT a, b FROM wide WHERE c = X       —— Project+Filter+Scan 应只读 {a, b, c}
--   2) SELECT *   FROM wide WHERE d > N        —— SELECT * 不裁剪
--   3) SELECT SUM(e) FROM wide                 —— 聚合路径：Scan 只需读 e
--   4) SELECT a, c FROM wide ORDER BY d        —— Sort 引用 d，下推要求保留 d
--   5) JOIN 后只取一侧的两列                  —— 两侧都应被裁剪
--   6) INSERT INTO other SELECT a, b FROM wide —— 子查询被裁剪

CREATE TABLE wide(id INT, name VARCHAR, dept VARCHAR, age INT, salary FLOAT);
CREATE TABLE other(id INT, name VARCHAR);

INSERT INTO wide VALUES
    (1, 'Alice',   'IT',     25, 7000.0),
    (2, 'Bob',     'IT',     30, 8500.0),
    (3, 'Charlie', 'HR',     22, 5000.0),
    (4, 'David',   'Sales',  28, 9000.0),
    (5, 'Eve',     'HR',     35, 9500.0);

-- 1) 投影 + 过滤
SELECT id, name FROM wide WHERE age = 25;
SELECT name, salary FROM wide WHERE dept = 'IT';

-- 2) SELECT * 不裁剪：结果应包含全部 5 列
SELECT * FROM wide WHERE age > 22;

-- 3) 聚合只引用 salary
SELECT SUM(salary) FROM wide;
SELECT dept, SUM(salary) FROM wide GROUP BY dept;

-- 4) ORDER BY 引入 age 列
SELECT id, name FROM wide ORDER BY age DESC LIMIT 2;

-- 5) JOIN 两侧各取一列
SELECT w.name, o.id FROM wide w JOIN other o ON w.id = o.id;

-- 6) INSERT ... SELECT 子查询
INSERT INTO other SELECT id, name FROM wide WHERE dept = 'IT';
SELECT * FROM other;

exit;
