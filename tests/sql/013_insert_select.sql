-- 14_insert_select.sql
-- 通过 SELECT 的结果填充目标表（复制/筛选/聚合后落盘）
-- 若编译器尚不支持 INSERT ... SELECT，部分语句可作为扩展点验证。

CREATE TABLE src(id INT, name VARCHAR, score FLOAT);
CREATE TABLE dst(id INT, name VARCHAR, score FLOAT);
CREATE TABLE high(id INT, name VARCHAR, score FLOAT);

INSERT INTO src VALUES
    (1, 'Alice',  88.5),
    (2, 'Bob',    91.0),
    (3, 'Charlie',76.5),
    (4, 'David',  95.5),
    (5, 'Eve',    82.0);

-- 单行指定列
INSERT INTO dst(id, name, score) VALUES (100, 'placeholder', 0.0);
SELECT * FROM dst;

-- 若 INSERT ... SELECT 已实现：把 src 整表复制到 dst
-- INSERT INTO dst SELECT * FROM src;
-- SELECT * FROM dst;

-- 若 INSERT ... SELECT 已实现：仅复制高分学生
-- INSERT INTO high SELECT id, name, score FROM src WHERE score >= 90;
-- SELECT * FROM high;

exit;