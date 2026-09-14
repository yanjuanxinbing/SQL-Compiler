-- 53_subquery_decorrelate.sql
-- U3-3 子查询去关联：相关 EXISTS/IN/ANY → SEMI/ANTI JOIN（Optimizer 改写），
-- 非相关子查询 → 物化缓存（ExecutionEngine），递归 CTE 迭代边界失效缓存。
-- 本文件只放「必然成功」的语句，保证 run_all_tests 的 exit code = 0；
-- 改写形态断言（EXPLAIN 中出现 Join(SEMI/ANTI)）与逐场景语义断言放在
-- storage_ut::TestSubqueryDecorrelation / TestSubqueryMaterialization /
-- TestRecursiveCteCacheInvalidation 里。

CREATE TABLE customers(id INT, name VARCHAR, city VARCHAR);
CREATE TABLE orders(id INT, cust_id INT, amount FLOAT);
INSERT INTO customers VALUES (1,'Alice','BJ'),(2,'Bob','SH'),(3,'Charlie','BJ'),(4,'David','GZ'),(5,'Eve','SH');
INSERT INTO orders VALUES (1,1,100.0),(2,1,50.0),(3,2,200.0),(4,3,80.0),(5,3,60.0),(6,5,300.0);

-- (1) 相关 EXISTS → SEMI（去关联改写后的结果必须与原语义一致）
SELECT name FROM customers c WHERE EXISTS (SELECT 1 FROM orders o WHERE o.cust_id = c.id) ORDER BY id;
-- (2) 相关 NOT EXISTS → ANTI，内层非相关子句留在 Filter
SELECT name FROM customers c WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.cust_id = c.id AND o.amount > 100) ORDER BY id;
-- (3) IN → SEMI（外列未限定、内层单表）
SELECT name FROM customers WHERE id IN (SELECT cust_id FROM orders WHERE amount >= 100) ORDER BY id;
-- (4) ANY → SEMI
SELECT name FROM customers WHERE id > ANY (SELECT cust_id FROM orders WHERE amount >= 100) ORDER BY id;
-- (5) 混合合取：city='BJ' AND EXISTS → 左子保留 Filter，结果 = 交集
SELECT name FROM customers WHERE city = 'BJ' AND EXISTS (SELECT 1 FROM orders o WHERE o.cust_id = customers.id) ORDER BY id;
-- (6) NOT IN 保守不改写（NULL 语义与 ANTI 不等价），结果仍正确
SELECT name FROM customers WHERE id NOT IN (SELECT cust_id FROM orders WHERE amount >= 200) ORDER BY id;
-- (7) 非相关 EXISTS → SEMI 无条件连接（右表非空 → 全部输出）
SELECT name FROM customers WHERE EXISTS (SELECT 1 FROM orders) ORDER BY id;
-- (8) 空表：EXISTS = 0 行、NOT EXISTS = 全部行
CREATE TABLE empty_t(x INT);
SELECT count(*) FROM customers WHERE EXISTS (SELECT 1 FROM empty_t);
SELECT count(*) FROM customers WHERE NOT EXISTS (SELECT 1 FROM empty_t);
-- (9) NULL 语义：外列 NULL 经 SEMI 按 UNKNOWN 过滤
CREATE TABLE cust_null(id INT, name VARCHAR);
INSERT INTO cust_null VALUES (1,'A'),(NULL,'B');
SELECT name FROM cust_null WHERE id IN (SELECT cust_id FROM orders) ORDER BY name;
-- (10) 嵌套相关子查询：两层均被递归去关联为内层 SEMI
SELECT name FROM customers c WHERE EXISTS (SELECT 1 FROM orders o WHERE o.cust_id = c.id AND EXISTS (SELECT 1 FROM customers c2 WHERE c2.id = o.cust_id)) ORDER BY id;
-- (11) 非相关标量子查询物化：5 外层行只跑 1 次子计划（\stats 中 materialize=1/hits=4）
SELECT name, (SELECT count(*) FROM orders) AS cnt FROM customers ORDER BY id;
-- (12) 相关标量子查询逐行求值、不入缓存（计数不变）
SELECT name, (SELECT max(amount) FROM orders o WHERE o.cust_id = c.id) AS mx FROM customers c ORDER BY id;
-- (13) 递归 CTE 迭代边界失效子查询缓存：序列 1,2,4,8 → cnt=4, mx=8
-- （若不清缓存，第 2 轮会复用第 1 轮物化的 max=1 → 序列 1,2,3,4,5）
WITH RECURSIVE acc AS (
    SELECT 1 AS n
    UNION ALL
    SELECT n + (SELECT max(acc.n) FROM acc) FROM acc WHERE n < 5
)
SELECT count(*) AS cnt, max(n) AS mx FROM acc;

\stats;
exit;
