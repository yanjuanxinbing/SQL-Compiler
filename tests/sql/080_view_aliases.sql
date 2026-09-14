-- 77_view_aliases.sql
-- BUG-4 [P1 中等]: VIEW 不能暴露列别名
-- --------------------------------------------------------------------------
-- 历史原因（已修复，见 SemanticAnalyzer.cpp::AnalyzeSelect）：
--   语义层在遇到 FROM view 时只跳过了"表存在性"检查，但没有把视图
--   SELECT 列表里的别名注册到 symbol_table_。导致 ORDER BY / WHERE
--   等后续子句引用别名时列存在性检查失败，报「column not found」。
--   Planner 的 TryExpandView 把视图替换为 derived_table，但语义分析
--   早在 Planner 之前就已跑完、错误已写出，所以必须在语义层处理。
--
-- 本测试覆盖：
--   1) 不带别名的视图（对照组）：始终工作。
--   2) 带别名的视图：SELECT *、ORDER BY 别名、WHERE 别名均通过。
--   3) 多列别名混合：在 SELECT/WHERE/ORDER BY 中穿插使用。
--   4) 表达式别名（`v + 1 AS next_v`）：ORDER BY 引用也能找到。
--   5) 视图内带 WHERE 子句：不影响外层 ORDER BY 别名解析。
--   6) DROP VIEW 后再访问视图：报错。
--   7) 物化视图（60_view_trigger）的别名也应当解析。

-- =====================================================================
-- 1) 不带别名（对照组）
-- =====================================================================
CREATE TABLE base_t (id INT PRIMARY KEY, v INT);
INSERT INTO base_t VALUES (1, 10), (2, 20), (3, 30);

CREATE VIEW v_no_alias AS SELECT v FROM base_t WHERE v > 15;
SELECT * FROM v_no_alias ORDER BY v;
DROP VIEW v_no_alias;

-- =====================================================================
-- 2) 带别名：SELECT *、ORDER BY 别名、WHERE 别名（BUG-4 主路径）
-- =====================================================================
CREATE VIEW v_alias AS SELECT id AS i, v AS val FROM base_t WHERE v < 25;
SELECT * FROM v_alias;                            -- 期望：2 行，i | val
SELECT * FROM v_alias ORDER BY i;                 -- 期望：按 i 升序
SELECT * FROM v_alias WHERE val > 15;             -- 期望：val=20 一行
SELECT * FROM v_alias ORDER BY val DESC;          -- 期望：val=20, val=10

-- =====================================================================
-- 3) 多列别名混合使用
-- =====================================================================
SELECT i, val FROM v_alias WHERE i < 10 ORDER BY val;
SELECT i FROM v_alias ORDER BY val, i;            -- 多键 ORDER BY

-- =====================================================================
-- 4) 表达式别名
-- =====================================================================
CREATE VIEW v_expr AS SELECT id AS i, v + 1 AS next_v FROM base_t;
SELECT * FROM v_expr ORDER BY next_v;
SELECT * FROM v_expr WHERE next_v > 11 ORDER BY next_v;
DROP VIEW v_expr;

-- =====================================================================
-- 5) 视图内带 WHERE 子句
-- =====================================================================
-- v_alias 内部的 WHERE v < 25 不影响外层 ORDER BY 别名解析。
SELECT val FROM v_alias ORDER BY val DESC LIMIT 1;
DROP VIEW v_alias;

-- =====================================================================
-- 6) DROP VIEW 后访问
-- =====================================================================
DROP VIEW IF EXISTS v_alias;
-- 期望：报错
SELECT * FROM v_alias;

-- =====================================================================
-- 7) 物化视图（60_view_trigger）的别名解析
-- =====================================================================
CREATE MATERIALIZED VIEW mv_alias AS
    SELECT id AS i, v AS val FROM base_t;
SELECT * FROM mv_alias ORDER BY i;
SELECT * FROM mv_alias WHERE val > 15;
DROP MATERIALIZED VIEW mv_alias;

DROP TABLE base_t;

exit;
