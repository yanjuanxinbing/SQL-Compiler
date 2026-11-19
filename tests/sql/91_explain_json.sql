-- Regression: EXPLAIN FORMAT JSON must emit *valid* JSON.
--
-- The WebUI visualization panel feeds the plan straight into
-- JSON.parse() (webui/backend/static/app.js :: renderPlan), so a missing
-- comma between object members or a double-quoted value silently breaks
-- the plan tree. The same JSON also travels to the browser inside the
-- [DEBUG_JSON_START]...[DEBUG_JSON_END] envelope as "plan_json".
--
-- Keep the expected output byte-exact: if the serializer changes shape,
-- this test has to be reviewed on purpose.
CREATE TABLE t(a INT, b TEXT);
EXPLAIN FORMAT JSON SELECT * FROM t WHERE a > 0;
