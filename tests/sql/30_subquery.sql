-- 30_subquery.sql
-- 子查询：标量子查询 / IN 子查询 / EXISTS / FROM 子查询 / 相关子查询

CREATE TABLE customers(
    id INT,
    name VARCHAR,
    city VARCHAR
);

CREATE TABLE orders(
    id INT,
    cust_id INT,
    amount FLOAT
);

INSERT INTO customers VALUES
    (1, 'Alice',   'BJ'),
    (2, 'Bob',     'SH'),
    (3, 'Charlie', 'BJ'),
    (4, 'David',   'GZ'),
    (5, 'Eve',     'SH');

INSERT INTO orders VALUES
    (1, 1, 100.0),
    (2, 1, 50.0),
    (3, 2, 200.0),
    (4, 3, 80.0),
    (5, 3, 60.0),
    (6, 5, 300.0);

-- 标量子查询：SELECT 列表中的单值子查询
SELECT id, name,
       (SELECT SUM(amount) FROM orders WHERE cust_id = customers.id) AS total
FROM customers;

-- 标量子查询：WHERE 中比较
SELECT name FROM customers
WHERE id = (SELECT cust_id FROM orders ORDER BY amount DESC LIMIT 1);

-- IN 子查询：WHERE col IN (SELECT ...)
SELECT name FROM customers
WHERE id IN (SELECT cust_id FROM orders WHERE amount >= 100);

-- NOT IN 子查询
SELECT name FROM customers
WHERE id NOT IN (SELECT cust_id FROM orders WHERE amount >= 200);

-- EXISTS 子查询
SELECT name FROM customers c
WHERE EXISTS (SELECT 1 FROM orders o WHERE o.cust_id = c.id);

-- NOT EXISTS 子查询
SELECT name FROM customers c
WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.cust_id = c.id AND o.amount > 100);

-- FROM 子查询：派生表
SELECT sub.cust_id, sub.cnt
FROM (SELECT cust_id, COUNT(*) AS cnt FROM orders GROUP BY cust_id) AS sub
WHERE sub.cnt >= 2;

-- 相关子查询：每行计算其订单总额
SELECT name,
       (SELECT COUNT(*) FROM orders o WHERE o.cust_id = c.id) AS order_cnt
FROM customers c;

-- ANY / ALL 子查询（部分方言）
SELECT name FROM customers
WHERE id > ANY (SELECT cust_id FROM orders WHERE amount >= 100);

-- 量化比较子查询：ANY / SOME / ALL
-- ANY：存在性量化。任意一行满足比较即 TRUE。
-- SOME：SQL 标准与 ANY 完全等价；测试解析与求值都对齐。
-- ALL：全称量化。所有非 NULL 行都满足比较才 TRUE；空集 → TRUE（vacuous）。
CREATE TABLE qcomp(id INT, val INT);
INSERT INTO qcomp VALUES (1,10),(2,20),(3,30);
-- val > ALL (SELECT val FROM qcomp WHERE id <= 1) -- 子查询返回 {10}
--   val=10: 10>10 FALSE；val=20/30: >10 TRUE → 返回 id=2,3
SELECT * FROM qcomp WHERE val > ALL (SELECT val FROM qcomp WHERE id <= 1);
-- val < ALL (SELECT val FROM qcomp WHERE id > 2)   -- 子查询返回 {30}
--   val=10/20: <30 TRUE；val=30: <30 FALSE → 返回 id=1,2
SELECT * FROM qcomp WHERE val < ALL (SELECT val FROM qcomp WHERE id > 2);
-- val >= ALL (SELECT val FROM qcomp WHERE id <= 2) -- 子查询返回 {10,20}
--   val=10: 10>=20 FALSE；val=20/30: 全部 >=10 且 >=20 → 返回 id=2,3
SELECT * FROM qcomp WHERE val >= ALL (SELECT val FROM qcomp WHERE id <= 2);
-- val = ALL (SELECT val FROM qcomp WHERE id <= 1)  -- 子查询返回 {10}
--   val=10: =10 TRUE → 返回 id=1
SELECT * FROM qcomp WHERE val = ALL (SELECT val FROM qcomp WHERE id <= 1);
-- val > SOME (SELECT val FROM qcomp WHERE id <= 1) -- SOME 等价 ANY：返回 id=2,3
SELECT * FROM qcomp WHERE val > SOME (SELECT val FROM qcomp WHERE id <= 1);
-- val > ALL (SELECT val FROM qcomp WHERE id > 10)  -- 子查询空集：vacuous TRUE
--   所有行通过
SELECT * FROM qcomp WHERE val > ALL (SELECT val FROM qcomp WHERE id > 10);
DROP TABLE qcomp;

-- 子查询嵌套
SELECT name FROM customers
WHERE id IN (
    SELECT cust_id FROM orders
    WHERE amount > (SELECT AVG(amount) FROM orders)
);

-- 嵌套聚合：外层 SUM/COUNT 套在内层 GROUP BY 之上。
-- 之前 derived_table 分支漏掉聚合路径导致 SUM(...) 被当标量函数求值返回 NULL。
CREATE TABLE nest_t1(id INT, grp VARCHAR, val INT);
INSERT INTO nest_t1 VALUES (1,'A',10),(2,'A',20),(3,'B',30),(4,'C',40);
SELECT grp, SUM(s) AS total FROM (
    SELECT grp, SUM(val) AS s FROM nest_t1 GROUP BY grp
) AS sub GROUP BY grp ORDER BY grp;
DROP TABLE nest_t1;

-- 60_query：内外层同名表的相关子查询。
-- 标准 SQL 规定内层 FROM t shadow 外层 FROM t，导致 `t.category = t.category`
-- 恒真、相关列失效、MAX(price) 返回全局最大。修复后内层给了 alias t2，
-- 裸名 `t` 被识别为「from_table 原名（非 alias）」，优先解析为外层 → 按
-- 外层 category 分桶返回各类 MAX。
CREATE TABLE cat_t(id INT, category VARCHAR, price FLOAT);
INSERT INTO cat_t VALUES (1,'Fruit',1.5),(2,'Fruit',3.0),(3,'Meat',10.0),(4,'Meat',5.0);
SELECT cat_t.id, cat_t.category, cat_t.price,
       (SELECT MAX(price) FROM cat_t t2 WHERE t2.category = cat_t.category) AS max_in_cat
FROM cat_t ORDER BY cat_t.id;

-- 60_query：相关子查询引用 GROUP BY 别名。`category AS cat` 的别名只活在
-- Project 输出位置，cmap 仅按基表列名建；修复前 BuildOuterBind 拿不到 `cat`、
-- 子查询 WHERE 全部 NULL 过滤 → MAX 返回 NULL。修复后 AggregateExecutor 的
-- cmap 被显式扩充为 `cat -> category 位置`，让 BuildOuterBind 能正确绑定。
SELECT category AS cat, COUNT(*) AS cnt,
       (SELECT MAX(price) FROM cat_t t2 WHERE t2.category = cat) AS max_price
FROM cat_t GROUP BY cat ORDER BY cat;
DROP TABLE cat_t;

-- 60_query：相关子查询限定符在 WalkExprForOuterRefs 中需识别为「from_table
-- 原名（被 alias 屏蔽）」—— 修复前 `t2.cat = t.cat` 中 t.cat 被当作内层元组，
-- 子查询退化为非相关并返回全局 MAX/SUM。
CREATE TABLE tq1(id INT, cat VARCHAR, val INT);
INSERT INTO tq1 VALUES (1,'A',10),(2,'A',20),(3,'B',30),(4,'B',30);
SELECT tq1.id, tq1.cat, tq1.val,
       (SELECT SUM(t2.val) FROM tq1 t2 WHERE t2.cat = tq1.cat) AS cat_sum
FROM tq1 ORDER BY tq1.id;
DROP TABLE tq1;

-- 60_query：嵌套相关子查询 —— 中间层 IN-list 子查询自身不被 IsSubqueryCorrelated
-- 直接识别（WHERE 内只有 qualified inner 列），但内层 SELECT 引用外层别名。
-- 修复前：中间层被缓存为非相关 → 返回首次评估的「Fruit」结果；嵌套子查询层
-- inner_tables 缺失也导致「cat」无法回退到外层 GROUP BY 别名。
-- 修复后：WalkExprForOuterRefs 递归遍历 SubqueryExpr 的内层 SELECT（让中间
-- 层正确识别为相关）；AggregateExecutor cmap 扩展 alias；EvaluateColumnRef
-- 三态解析（alias 必内层 / from_table 原名可外层 / 其他外层）；EvaluateSubquery
-- 始终预挂「父 bind + 当前行」穿透嵌套链。
CREATE TABLE nest_products(id INT, category VARCHAR, price FLOAT);
INSERT INTO nest_products VALUES (1,'Fruit',2.0),(2,'Fruit',5.0),
                                (3,'Meat',15.0),(4,'Meat',20.0),
                                (5,'Vegetable',7.0);
CREATE TABLE nest_orders(id INT, product_id INT, qty INT);
INSERT INTO nest_orders VALUES (1,1,3),(2,1,2),(3,3,5),(4,5,2),(5,2,4),(6,4,1);
SELECT category AS cat, COUNT(*) AS cnt,
       (SELECT SUM(o.qty) FROM nest_orders o WHERE o.product_id IN
            (SELECT id FROM nest_products WHERE category = cat)
       ) AS total_qty_for_cat
FROM nest_products GROUP BY cat ORDER BY cat;
DROP TABLE nest_orders;
DROP TABLE nest_products;

-- 60_query：相关子查询中使用裸列名引用外层列（内层表给了 alias 屏蔽了裸名）。
-- 修复前：`t2.cat = cat` 中 cat 被内层 cmap 解析为内层 row 值，WHERE 退化为
-- 恒真、SUM 返回全局合计。修复后：FilterExecutor 的 evaluator 在内层有 alias
-- 时按用户意图把裸列名 cat 优先解析为外层；AggregateExecutor 求 SUM(val) 实参
-- 时关闭该偏好（否则 val 也会被解析为外层，导致按外层 row 重复累加）。
CREATE TABLE bare_t(id INT, cat VARCHAR, val INT);
INSERT INTO bare_t VALUES (1,'A',10),(2,'A',20),(3,'B',30),(4,'B',30);
SELECT bare_t.id, bare_t.cat, bare_t.val,
       (SELECT SUM(val) FROM bare_t t2 WHERE t2.cat = cat) AS s
FROM bare_t ORDER BY bare_t.id;
DROP TABLE bare_t;

-- 60_query：相关子查询另一写法 —— 裸列名在左侧、from_table 原名在右侧：
--   WHERE cat = t.cat
-- 与上一条的区别：裸 cat 在比较的另一侧（左侧 t.cat 用的是 from_table 原名
// 而非 alias t2）。启发式需进一步细化：仅当「另一侧用了内层 alias」才把裸列
-- 名优先解析为外层；「另一侧用 from_table 原名」时不触发，让裸列名按标准 SQL
-- 默认解析为内层 row。这样 WHERE = inner.cat = outer.t.cat 正确过滤。
CREATE TABLE bare_t2(id INT, cat VARCHAR, val INT);
INSERT INTO bare_t2 VALUES (1,'A',10),(2,'A',20),(3,'B',30),(4,'B',30);
SELECT bare_t2.id, bare_t2.cat, bare_t2.val,
       (SELECT SUM(t2.val) FROM bare_t2 t2 WHERE cat = bare_t2.cat) AS s
FROM bare_t2 ORDER BY bare_t2.id;
DROP TABLE bare_t2;

exit;