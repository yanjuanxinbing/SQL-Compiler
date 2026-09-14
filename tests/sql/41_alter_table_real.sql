-- 41_alter_table_real.sql
-- 验证 ALTER TABLE 真正改写目录与行字节，而不再是无副作用的 no-op。

CREATE TABLE t(id INT PRIMARY KEY, score INT);
INSERT INTO t VALUES (1, 80), (2, 95);

-- ADD COLUMN: 新列追加；现有行的该列值由 NULL 回填。
ALTER TABLE t ADD COLUMN name VARCHAR(50);
SELECT id, score, name FROM t ORDER BY id;

-- DROP COLUMN: 被删列从元数据与行字节中同时移除。
ALTER TABLE t DROP COLUMN score;
SELECT id, name FROM t ORDER BY id;

-- DROP 之后，原列已不存在；直接 SELECT 应报「column not found」。
SELECT score FROM t;

-- RENAME TO: 表名迁移到新键。
ALTER TABLE t RENAME TO t_renamed;

-- 旧表名 t 不再可见。
SELECT * FROM t;

-- 新表名 t_renamed 可正常读取。
SELECT * FROM t_renamed ORDER BY id;

-- MODIFY COLUMN: 同类型间安全转换。FLOAT -> INT 会截断小数，但当前列原值都是
-- INTEGER，因此不会丢失信息。语法通过 + 字节重写完成。
ALTER TABLE t_renamed ADD COLUMN score INT;
INSERT INTO t_renamed VALUES (3, 'carol', 70);
ALTER TABLE t_renamed MODIFY COLUMN score BIGINT;
SELECT * FROM t_renamed ORDER BY id;

exit;