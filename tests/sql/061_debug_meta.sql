-- Phase 1.5: debug meta-command smoke.
-- This script only feeds SQL into the engine; the \.tokens / \.ast / \.plan
-- meta-commands are exercised separately by run_debug_meta.bat on the REPL.
CREATE TABLE foo(id INT, val INT);
INSERT INTO foo(id,val) VALUES (1,10),(2,20);
SELECT id, val FROM foo WHERE id > 1;
