-- 83_cte_select_star.sql
-- BUG 修复：CTE under `SELECT *` loses VARCHAR column content
-- --------------------------------------------------------------------------
-- 背景：
--   ProjectExecutor 对 `SELECT *` 的输出元组形态：当 select_list[0] 是
--   function_name=="*" 时直接透传 underlying tuple，不会追加 [select ++ underlying] 形式。
--   CteDefineExecutor 物化 CTE 时无条件按 `cols.size()` 截断行；
--   对 `SELECT *`，cols.size()=3（id/name/mgr），但 materialised row 宽度也是 3，
--   截断后行内容本身应该保留。但实际运行中 name 列变成空字符串。
-- 假设：cols.size() 错位（被推到 select_list 的 `*` 走错分支），导致 trim 截到了错的列；
-- 或 cols 推导虽然正确但 ProjectExecutor 的透传 + trim 顺序上仍然错位。

CREATE TABLE emp (id INT, name VARCHAR(50), mgr INT);
INSERT INTO emp VALUES (1,'CEO',NULL),(2,'VP1',1),(3,'VP2',1);

-- Bug: CTE 下 SELECT * 应保留 name
WITH t AS (SELECT * FROM emp) SELECT * FROM t;

-- Bug: CTE 下单独 SELECT name 应保留 name
WITH t AS (SELECT * FROM emp) SELECT name FROM t;

-- 对照：显式列出列名时 OK
WITH t AS (SELECT id, name FROM emp) SELECT * FROM t;

-- 对照：subquery 形态 OK
SELECT * FROM (SELECT * FROM emp) AS subq;

-- 多行 VARCHAR 测试
CREATE TABLE msg (id INT, body VARCHAR(100));
INSERT INTO msg VALUES (1,'hello world'),(2,'goodbye');
WITH m AS (SELECT * FROM msg) SELECT * FROM m;

exit;