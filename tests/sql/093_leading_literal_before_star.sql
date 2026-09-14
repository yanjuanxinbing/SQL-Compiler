-- 87_leading_literal_before_star.sql
-- Bug 12 regression: leading literal before `*` in SELECT list causes column shift.
--
-- Original repro: `SELECT 'X' AS c, * FROM p ORDER BY id` was returning
-- `X | 1 | 1 | USA` for every row because the planner passed the SELECT list
-- [LiteralExpr('X'), FunctionCallExpr('*')] verbatim to the projection; the
-- projection treated `*` as a function call (EvaluateFunctionCall returns 1
-- for unknown function name '*') and the underlying scan tuple was appended
-- after, shifting the data left by one column.
--
-- Fix: Planner::ExpandSelectStarInList rewrites the SELECT list in place so
-- that the `*` is replaced by a ColumnRefExpr for every column of the FROM
-- table (and any joined tables). The output tuple layout then matches the
-- (expanded) select list and the column_index_map is consistent.

CREATE TABLE p(id INT, country TEXT, credit REAL);
INSERT INTO p VALUES (1,'USA',5000),(2,'USA',3000),(3,'UK',2000);

-- The original repro: literal at the START of the SELECT list.
SELECT 'X' AS c, * FROM p ORDER BY id;

-- Literal at the END — same projection, different order.
SELECT *, 'X' AS c FROM p ORDER BY id;

-- Literal in the MIDDLE (no * present; sanity check, was already correct).
SELECT id, 'X' AS c, country, credit FROM p ORDER BY id;

-- Interaction with WHERE: `*` after a literal still has to be expanded.
SELECT 'X' AS c, * FROM p WHERE id > 1 ORDER BY id;

-- Interaction with GROUP BY: same expansion is required.
SELECT 'X' AS c, * FROM p GROUP BY id ORDER BY id;

-- ORDER BY that re-orders rows (not by id) — projection still has to align.
SELECT 'X' AS c, * FROM p ORDER BY country;

-- Sanity: SELECT * without any other expression still works.
SELECT * FROM p ORDER BY id;

-- Sanity: SELECT literal without * still works.
SELECT 'X' AS c FROM p ORDER BY id;

-- No-star: explicit column list works regardless of leading literal.
SELECT 'X' AS c, id, country, credit FROM p ORDER BY id;

-- Derived table source: same expansion is required.
SELECT 'X' AS c, * FROM (SELECT id, country FROM p) AS sub ORDER BY id;

exit;
