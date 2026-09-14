-- 88_qualified_star_join.sql
-- Bug 13 regression: `t.*` / `q.*` table-qualified stars must expand to
-- ONLY the qualified table's columns, not all joined tables' columns.
--
-- Original repro (Bug 12 fix accidentally regressed):
--   `SELECT p.* FROM p JOIN q ON ...` previously expanded to all columns of
--   from_table + joins (i.e., p.id, p.country, p.credit, q.pid, q.x) because
--   the parser dropped the `p.` qualifier at `src/parser/Parser.cpp:2662-2664`,
--   making `p.*` indistinguishable from bare `*` in ExpandSelectStarInList.
--
-- Fix:
--   - Parser now sets FunctionCallExpr::table_qualifier on `t.*` AST nodes.
--   - Planner::ExpandSelectStarInList reads the qualifier and expands only
--     the matching table's columns (matched by real table name or alias).

CREATE TABLE p(id INT, country TEXT, credit REAL);
CREATE TABLE q(pid INT, x REAL);
INSERT INTO p VALUES (1,'USA',5000),(2,'USA',3000);
INSERT INTO q VALUES (1, 100),(2, 200);

-- 1. Baseline: explicit column list works correctly (5 cols).
SELECT 'j' AS j, p.id, p.country, p.credit, q.x FROM p JOIN q ON q.pid = p.id ORDER BY p.id;

-- 2. `p.*` after a literal — expect 4 cols (j, id, country, credit).
SELECT 'j' AS j, p.* FROM p JOIN q ON q.pid = p.id ORDER BY p.id;

-- 3. `p.*, q.x` — expect 5 cols (j, id, country, credit, x); q.x is in q only.
SELECT 'j' AS j, p.*, q.x FROM p JOIN q ON q.pid = p.id ORDER BY p.id;

-- 4. `p.*, q.x` without literal — expect 4 cols.
SELECT p.*, q.x FROM p JOIN q ON q.pid = p.id ORDER BY p.id;

-- 5. `q.*` — expect 2 cols (pid, x).
SELECT q.* FROM p JOIN q ON q.pid = p.id ORDER BY q.pid;

-- 6. Explicit column reference works (3 cols).
SELECT 'j' AS j, p.id, q.x FROM p JOIN q ON q.pid = p.id ORDER BY p.id;

-- 7. `p.*, q.pid` — expect 5 cols (j, id, country, credit, pid).
SELECT 'j' AS j, p.*, q.pid FROM p JOIN q ON q.pid = p.id ORDER BY p.id;

-- 8. `p.* FROM p` (no JOIN) — expect 3 cols.
SELECT p.* FROM p ORDER BY id;

-- 9. `q.*, p.*` — expect 5 cols (q's cols first then p's cols; no dedupe).
SELECT q.*, p.* FROM p JOIN q ON q.pid = p.id ORDER BY q.pid;

-- 10. `p.*` with table alias — qualifier matches alias, not real name.
SELECT 'j' AS j, pp.* FROM p AS pp JOIN q ON q.pid = pp.id ORDER BY pp.id;

-- 11. Bug 12 regression: bare `*` still expands to from_table + joins.
SELECT 'j' AS j, p.id, p.country, p.credit, q.x FROM p JOIN q ON q.pid = p.id ORDER BY p.id;

exit;