-- 70_mv_schema.sql
--
-- Materialized View schema-inference regressions:
--   * Empty source table  -> MV must have the right per-column types (no VARCHAR fallback)
--   * Mixed-type projection (INT / VARCHAR / DATE) -> all types preserved
--   * Computed expressions -> types inferred from operator (INT / VARCHAR)
--   * REFRESH after rows added -> schema unchanged + rows present
--   * Schema drift detection -> REFRESH on a drifted schema errors clearly
--
-- The prior implementation (V0) used a "probe first row" approach:
--   child->Next(&t) and looked at v.GetType(). When the source table was
-- empty, the executor fell back to a single VARCHAR column called "col0" —
-- wrong, because the MV should mirror the SELECT's projection types even
-- when there's no data to inspect.
--
-- Each block verifies a specific case. Each test run starts from a fresh .db
-- (run_all_tests.bat deletes the per-test db before invoking the REPL), so
-- cleanup at the bottom is best-effort.

-- ============================================================
-- Part A: Empty source table
-- ============================================================
CREATE TABLE empty_src (id INT, name VARCHAR(40), created DATE);
-- Note: no INSERT. The source table is genuinely empty.

CREATE MATERIALIZED VIEW mv_empty AS SELECT id, name, created FROM empty_src;

-- DESCRIBE via the public view name (Planner expands to backing table
-- automatically). The schema MUST show three columns with the original
-- types — NOT a single VARCHAR placeholder.
SHOW COLUMNS FROM mv_empty;

-- ============================================================
-- Part B: Multi-type projection (INT / VARCHAR / DATE)
-- ============================================================
-- Avoid 'day' / 'date' column names — those are reserved in this dialect.
CREATE TABLE mix_src (id INT, label VARCHAR(20), d DATE);
INSERT INTO mix_src VALUES (1, 'a', DATE '2024-01-15'),
                            (2, 'b', DATE '2024-02-20'),
                            (3, 'c', DATE '2024-03-25');

CREATE MATERIALIZED VIEW mv_mix AS SELECT id, label, d FROM mix_src;
SHOW COLUMNS FROM mv_mix;

-- The MV must return all rows faithfully.
SELECT * FROM mv_mix ORDER BY id;

-- ============================================================
-- Part C: Computed expressions
-- ============================================================
-- id + 1 -> INT; name || '!' -> VARCHAR (CONCAT operator returns VARCHAR);
-- LENGTH(name) -> INT (string-length builtins are INT in our dialect).
CREATE MATERIALIZED VIEW mv_calc AS
  SELECT id + 1 AS x, label || '!' AS y, LENGTH(label) AS l
  FROM mix_src;
SHOW COLUMNS FROM mv_calc;
SELECT * FROM mv_calc ORDER BY x;

-- ============================================================
-- Part D: REFRESH after rows added
-- ============================================================
-- Create MV on an empty table, INSERT new rows, REFRESH — verify the schema
-- is preserved (not silently re-inferred to a different type) AND the rows
-- appear.
CREATE TABLE refresh_src (id INT, v INT);
CREATE MATERIALIZED VIEW mv_refresh AS SELECT id, v FROM refresh_src;
-- Initially empty: schema must already reflect INT,INT.
SHOW COLUMNS FROM mv_refresh;
SELECT * FROM mv_refresh;

-- Now add rows and refresh; schema must stay INT,INT.
INSERT INTO refresh_src VALUES (10, 100), (20, 200), (30, 300);
ALTER MATERIALIZED VIEW mv_refresh REFRESH;
SHOW COLUMNS FROM mv_refresh;
SELECT * FROM mv_refresh ORDER BY id;

-- ============================================================
-- Part E: Schema drift detection
-- ============================================================
-- After ALTER TABLE ... MODIFY COLUMN on a column the MV references, the
-- source's projection type changes — REFRESH must surface a "schema drift
-- detected" error and refuse to silently rewrite the backing table. V1
-- follows PostgreSQL: schema changes require DROP + CREATE.
ALTER TABLE refresh_src MODIFY COLUMN v FLOAT;

-- REFRESH must error out (the error message should mention 'schema drift').
ALTER MATERIALIZED VIEW mv_refresh REFRESH;

exit;