-- 39_ddl_extensions.sql
-- DDL 扩展：ALTER TABLE / 列级 CHECK / 列级 DEFAULT

CREATE TABLE t(id INT, score INT);
INSERT INTO t VALUES (1, 80), (2, 95);

ALTER TABLE t ADD COLUMN name VARCHAR(50);

ALTER TABLE t DROP COLUMN score;

ALTER TABLE t RENAME TO t_renamed;

ALTER TABLE t MODIFY COLUMN score BIGINT;

CREATE TABLE t_check(
    id INT PRIMARY KEY,
    score INT CHECK (score >= 0 AND score <= 100)
);

CREATE TABLE t_default(
    id INT PRIMARY KEY,
    status VARCHAR DEFAULT 'active',
    created_at INT DEFAULT 0
);

SELECT * FROM t ORDER BY id;

exit;
