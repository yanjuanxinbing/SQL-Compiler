-- 84_set_op_in_derived_table.sql
-- BUG 6 回归测试：UNION / INTERSECT / EXCEPT 嵌套在 FROM (...) AS alias 派生表里
-- 旧 parser 在 ParseFromClause 把内层 SetOperationStatement 通过
-- std::static_pointer_cast<SelectStatement> 强转到 SelectStatement&，
-- 但两者没有继承关系（都只继承自 Statement）—— 那是 UB。
-- planner 用 *stmt.derived_table 读 SelectStatement 字段，从 SetOp
-- 的内存布局里读 garbage，最终 SIGSEGV。
-- 修复：
--   1) parser 改用 dynamic_pointer_cast，按动态类型分流到
--      derived_table / 新增的 derived_set_op 字段；
--   2) planner 在 PlanSelect 头部检查 derived_set_op，并 dispatch 到
--      PlanSetOperation；走的是同一 SeqScanNode 占位 + children[0]=子计划
--      的派生表协议，ExecutionEngine 透明改走子计划。

-- 1) UNION 在派生表里
SELECT * FROM (SELECT 1 AS i UNION SELECT 2 AS i) AS gen ORDER BY i;

-- 2) INTERSECT 在派生表里
SELECT * FROM (SELECT 1 AS i INTERSECT SELECT 1 AS i) AS gen;

-- 3) EXCEPT 在派生表里
SELECT * FROM (SELECT 1 AS i EXCEPT SELECT 2 AS i) AS gen;

-- 4) UNION ALL 在派生表里
SELECT * FROM (SELECT 1 AS i UNION ALL SELECT 2 AS i) AS gen ORDER BY i;

-- 5) 派生表 + 外层 WHERE / 列投影 / ORDER BY
SELECT gen.i + 10 AS y
FROM (SELECT 1 AS i UNION SELECT 2 AS i) AS gen
WHERE gen.i > 0
ORDER BY y;

-- 6) 嵌套派生表
SELECT * FROM (
    SELECT * FROM (SELECT 1 AS a UNION SELECT 2 AS a) AS inner_gen
) AS outer_gen ORDER BY a;

-- 7) 三路 UNION 在派生表里
SELECT * FROM (SELECT 1 AS i UNION SELECT 2 AS i UNION SELECT 3 AS i) AS gen ORDER BY i;

-- 8) CTE + 集合运算在派生表里
CREATE TABLE t(x INT);
INSERT INTO t VALUES (1),(2),(3);
WITH a AS (SELECT x FROM t WHERE x = 1), b AS (SELECT x FROM t WHERE x = 2)
SELECT * FROM (SELECT x FROM a UNION SELECT x FROM b) AS gen ORDER BY x;

exit;
