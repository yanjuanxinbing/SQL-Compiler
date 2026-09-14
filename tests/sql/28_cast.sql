-- 28_cast.sql
-- CAST / 类型转换：字符串与数值互转、显式类型提升
-- CAST(expr AS type) 是 SQL 标准写法；部分方言也支持 ::type 简写。

CREATE TABLE mixed(
    id INT,
    name VARCHAR,
    score FLOAT,
    created VARCHAR
);

INSERT INTO mixed VALUES
    (1, 'Alice', 88.5, '2024-01-15'),
    (2, 'Bob',   91.0, '2024-02-20'),
    (3, 'Carol', 76.5, '2024-03-10');

-- 字符串 -> 整数
SELECT id, name, CAST('42' AS INT) AS parsed_int FROM mixed;

-- 字符串 -> 浮点
SELECT id, CAST('3.14' AS FLOAT) AS parsed_float FROM mixed;

-- 整数 -> 浮点
SELECT id, CAST(id AS FLOAT) AS id_as_float FROM mixed;

-- 浮点 -> 字符串
SELECT id, CAST(score AS VARCHAR) AS score_text FROM mixed;

-- 整数 -> 字符串
SELECT id, CAST(id AS VARCHAR) AS id_text FROM mixed;

-- 表达式中混合类型：INT + FLOAT 隐式提升
SELECT id, score, id + score AS promoted FROM mixed;

-- CAST 出现在 WHERE 子句中
SELECT id, name FROM mixed WHERE CAST(score AS INT) >= 80;

-- CAST 在 ORDER BY 中
SELECT id, name FROM mixed ORDER BY CAST(score AS INT) DESC;

-- CAST 与 NULL
SELECT id, CAST(NULL AS INT) AS null_int FROM mixed;

-- 嵌套 CAST
SELECT id, CAST(CAST(id AS VARCHAR) AS INT) AS roundtrip FROM mixed;

exit;