-- 22_boundary.sql
-- 边界场景：空表、单行、大量行、极值、字符串特殊字符

CREATE TABLE empty_t(id INT, name VARCHAR);
CREATE TABLE single_t(id INT, val INT);
CREATE TABLE big_t(id INT, group_id INT, score FLOAT);

-- 空表 SELECT
SELECT * FROM empty_t;
SELECT COUNT(*) FROM empty_t;
SELECT COUNT(*) FROM empty_t GROUP BY id;

-- 单行
INSERT INTO single_t VALUES (1, 100);
SELECT * FROM single_t;
SELECT * FROM single_t WHERE id = 1;
SELECT * FROM single_t WHERE id = 2;

-- 大量行
INSERT INTO big_t VALUES
    (1,  1, 10.0),
    (2,  1, 20.0),
    (3,  1, 30.0),
    (4,  2, 15.0),
    (5,  2, 25.0),
    (6,  2, 35.0),
    (7,  3,  5.0),
    (8,  3, 45.0),
    (9,  3, 55.0),
    (10, 1, 60.0);

SELECT COUNT(*) FROM big_t;

-- 极值边界
SELECT MAX(score), MIN(score), AVG(score), SUM(score) FROM big_t;

-- 字符串含特殊字符：引号转义、嵌入空格
CREATE TABLE str_t(id INT, msg VARCHAR);
INSERT INTO str_t VALUES (1, 'hello world');
INSERT INTO str_t VALUES (2, 'it''s a test');  -- SQL 标准双单引号转义
INSERT INTO str_t VALUES (3, '中文 + emoji-like :)');
INSERT INTO str_t VALUES (4, '');
INSERT INTO str_t VALUES (5, 'line1' || 'line2');  -- 部分方言支持

SELECT * FROM str_t;

-- ORDER BY + LIMIT 边界
SELECT * FROM big_t ORDER BY id LIMIT 1;
SELECT * FROM big_t ORDER BY id LIMIT 0, 1;
SELECT * FROM big_t ORDER BY id LIMIT 100;  -- 超出范围
SELECT * FROM big_t ORDER BY id LIMIT 9, 5; -- 起始位置接近末尾

exit;