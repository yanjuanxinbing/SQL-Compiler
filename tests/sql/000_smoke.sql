-- 00_smoke.sql
-- 冒烟测试：最基本功能是否正常

CREATE TABLE t(id INT, name VARCHAR, age INT);
INSERT INTO t VALUES (1, 'Alice', 20);
INSERT INTO t VALUES (2, 'Bob', 25);
SELECT * FROM t;
exit;
