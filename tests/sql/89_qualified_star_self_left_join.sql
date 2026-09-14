-- 89_qualified_star_self_left_join.sql
-- Bug 14 regression: `e.*` in a self-LEFT JOIN used to expand to columns
-- qualified by the *real* table name (e.g., `emp.id`), causing cmap
-- collisions in BuildCombinedColumnIndexMap (second occurrence of `emp.X`
-- overwrote the first). Result: every `e.*` row read from the right-hand
-- side of the join.
--
-- Fix:
--   - Planner::ExpandSelectStarInList emits ColumnRefExprs using the
--     matched qualifier key (alias when present, else real name). For
--     `e.*` in `FROM emp e LEFT JOIN emp m`, the expansion becomes
--     `e.id, e.name, e.mgr_id` (alias-qualified) instead of
--     `emp.id, emp.name, emp.mgr_id`.
--   - Same logic applied to bare `*` over a self-join (uses alias when
--     the table entry has one distinct from the real name).

CREATE TABLE emp(id INT, name TEXT, mgr_id INT);
INSERT INTO emp VALUES (1,'CEO',NULL),(2,'VP',1),(3,'Dev',2);

-- 1. Baseline (explicit columns) — must be correct on every fix.
SELECT 'B' AS b, e.id, e.name, e.mgr_id, m.name AS mgr
FROM emp e LEFT JOIN emp m ON e.mgr_id = m.id ORDER BY e.id;

-- 2. The original failing query.
SELECT 'X' AS x, e.*, m.name AS mgr
FROM emp e LEFT JOIN emp m ON e.mgr_id = m.id ORDER BY e.id;

-- 3. `e.*` alone — only left-side columns, m side rows still padded with NULL.
SELECT e.* FROM emp e LEFT JOIN emp m ON e.mgr_id = m.id ORDER BY e.id;

-- 4. `e.*, m.name` without the leading literal.
SELECT e.*, m.name FROM emp e LEFT JOIN emp m ON e.mgr_id = m.id ORDER BY e.id;

-- 5. Leading literal + e.* (no m reference).
SELECT 'X' AS x, e.* FROM emp e LEFT JOIN emp m ON e.mgr_id = m.id ORDER BY e.id;

-- 6. `m.*` — right side; CEO row must remain in the output (padded NULLs).
SELECT m.* FROM emp e LEFT JOIN emp m ON e.mgr_id = m.id ORDER BY e.id;

-- 7. `e.*, m.name` with explicit id — duplicate id column (sanity).
SELECT e.id, e.*, m.name FROM emp e LEFT JOIN emp m ON e.mgr_id = m.id ORDER BY e.id;

-- 8. `e.*, m.*` — both sides expanded; expect 6 columns.
SELECT e.*, m.* FROM emp e LEFT JOIN emp m ON e.mgr_id = m.id ORDER BY e.id;

-- 9. INNER JOIN variant of (2); CEO row excluded since e.mgr_id IS NULL.
SELECT 'X' AS x, e.*, m.name FROM emp e INNER JOIN emp m ON e.mgr_id = m.id ORDER BY e.id;

-- 10. Bare `*` over a self-join — expanded entries must each use the
--     correct alias (e then m), so the cmap has e.X and m.X as distinct
--     keys, not two colliding emp.X entries.
SELECT e.*, '|', m.* FROM emp e LEFT JOIN emp m ON e.mgr_id = m.id ORDER BY e.id;

exit;