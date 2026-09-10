-- 36_index_scan.sql
-- 索引扫描与全表扫描的等价性：同一批数据、同一组查询，
-- 有索引的表与无索引的表必须返回相同结果（用 ORDER BY 固定顺序后比对）

CREATE TABLE with_idx(id INT PRIMARY KEY, name VARCHAR(20) NOT NULL, score INT);
CREATE TABLE no_idx(id INT, name VARCHAR(20) NOT NULL, score INT);

INSERT INTO with_idx VALUES (10, 'j', 100);
INSERT INTO with_idx VALUES (20, 't', 200);
INSERT INTO with_idx VALUES (30, 'th', 300);
INSERT INTO with_idx VALUES (40, 'f', 400);
INSERT INTO with_idx VALUES (50, 'fi', 500);
INSERT INTO no_idx VALUES (10, 'j', 100);
INSERT INTO no_idx VALUES (20, 't', 200);
INSERT INTO no_idx VALUES (30, 'th', 300);
INSERT INTO no_idx VALUES (40, 'f', 400);
INSERT INTO no_idx VALUES (50, 'fi', 500);

-- 等值
SELECT * FROM with_idx WHERE id = 30 ORDER BY id;
SELECT * FROM no_idx WHERE id = 30 ORDER BY id;
-- 闭区间
SELECT * FROM with_idx WHERE id >= 20 AND id <= 40 ORDER BY id;
SELECT * FROM no_idx WHERE id >= 20 AND id <= 40 ORDER BY id;
-- 开区间
SELECT * FROM with_idx WHERE id > 20 AND id < 50 ORDER BY id;
SELECT * FROM no_idx WHERE id > 20 AND id < 50 ORDER BY id;
-- BETWEEN
SELECT * FROM with_idx WHERE id BETWEEN 10 AND 30 ORDER BY id;
SELECT * FROM no_idx WHERE id BETWEEN 10 AND 30 ORDER BY id;
-- 单边
SELECT * FROM with_idx WHERE id < 25 ORDER BY id;
SELECT * FROM no_idx WHERE id < 25 ORDER BY id;
-- 索引区间 + 残余谓词
SELECT * FROM with_idx WHERE id > 10 AND score < 400 ORDER BY id;
SELECT * FROM no_idx WHERE id > 10 AND score < 400 ORDER BY id;
-- 空结果
SELECT * FROM with_idx WHERE id = 999;
SELECT * FROM no_idx WHERE id = 999;
-- 聚合
SELECT COUNT(*) FROM with_idx WHERE id >= 30;
SELECT COUNT(*) FROM no_idx WHERE id >= 30;
-- 非索引列谓词（应回退全表扫描）
SELECT * FROM with_idx WHERE score = 300 ORDER BY id;
SELECT * FROM no_idx WHERE score = 300 ORDER BY id;

exit;
