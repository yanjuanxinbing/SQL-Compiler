-- 35_index_lifecycle.sql
-- 索引生命周期：回填、DML 同步、DROP INDEX、TRUNCATE、DROP TABLE

CREATE TABLE lc(id INT PRIMARY KEY, code VARCHAR(20) NOT NULL, qty INT);
INSERT INTO lc VALUES (1, 'aaa', 10);
INSERT INTO lc VALUES (2, 'bbb', 20);
INSERT INTO lc VALUES (3, 'ccc', 30);

-- 在已有数据的表上建索引，应把存量数据回填进去
CREATE INDEX idx_lc_code ON lc(code);
SELECT * FROM lc WHERE code = 'bbb';

-- UPDATE 后索引应随之更新：旧键查不到，新键查得到
UPDATE lc SET code = 'zzz' WHERE id = 2;
SELECT * FROM lc WHERE code = 'zzz';
SELECT * FROM lc WHERE code = 'bbb';

-- DELETE 后索引项应一并摘除，不能留下幽灵行
DELETE FROM lc WHERE id = 3;
SELECT * FROM lc WHERE code = 'ccc';
SELECT * FROM lc ORDER BY id;

-- 删除后重新插入同一个键
INSERT INTO lc VALUES (3, 'ccc', 33);
SELECT * FROM lc WHERE code = 'ccc';

-- DROP INDEX 后查询回退到全表扫描，结果不变
DROP INDEX idx_lc_code;
SELECT * FROM lc WHERE code = 'ccc';
DROP INDEX IF EXISTS idx_lc_code;

-- TRUNCATE 后索引内容清空，主键可以重新使用
TRUNCATE TABLE lc;
SELECT * FROM lc;
INSERT INTO lc VALUES (1, 'aaa', 1);
SELECT * FROM lc WHERE id = 1;

-- DROP TABLE 应回收索引页，重建同名表不受残留影响
DROP TABLE lc;
CREATE TABLE lc(id INT PRIMARY KEY, code VARCHAR(20) NOT NULL);
SELECT * FROM lc;
INSERT INTO lc VALUES (1, 'fresh');
SELECT * FROM lc WHERE id = 1;

exit;
