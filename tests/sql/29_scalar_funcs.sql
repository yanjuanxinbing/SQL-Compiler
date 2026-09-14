-- 29_scalar_funcs.sql
-- 标量函数：字符串 / 数学 / 日期
-- 这些函数对每行返回单个值（与聚合函数相反），可与普通表达式混用。

CREATE TABLE goods(
    id INT,
    name VARCHAR,
    price FLOAT,
    stock INT,
    on_sale INT         -- 0/1 标志
);

INSERT INTO goods VALUES
    (1, 'Apple',    3.5,  100, 1),
    (2, 'Banana',   1.2,  50,  0),
    (3, 'Cherry',   8.0,  20,  1),
    (4, 'Donut',    2.0,  0,   0);

-- 字符串函数
SELECT id,
       UPPER(name)        AS up,
       LOWER(name)        AS low,
       LENGTH(name)       AS len,
       SUBSTR(name, 1, 3) AS prefix,
       TRIM(name)         AS trimmed,
       REPLACE(name, 'a', '@') AS replaced
FROM goods;

-- 字符串拼接 + 函数组合
SELECT id, UPPER(SUBSTR(name, 1, 1)) || LOWER(SUBSTR(name, 2)) AS initcap FROM goods;

-- 数学函数
SELECT id, price,
       ROUND(price)    AS rounded,
       CEIL(price)     AS ceiled,
       FLOOR(price)    AS floored,
       ABS(price - 5)  AS dist_from_5,
       POWER(price, 2) AS sq,
       MOD(stock, 3)   AS mod3
FROM goods;

-- 日期/时间函数（与字符串混用）
SELECT id, name,
       YEAR('2024-03-15')  AS yr,
       MONTH('2024-03-15') AS mo,
       DAY('2024-03-15')   AS dd,
       NOW()               AS ts
FROM goods
WHERE id = 1;

-- 函数嵌套
SELECT id, ROUND(AVG(price)) AS avg_rounded
FROM goods
GROUP BY id;  -- 每组单值，演示嵌套标量函数

-- 标量函数与算术混用
SELECT id, name, ROUND(price * 1.1, 2) AS with_tax FROM goods;

-- 在 WHERE 子句中使用标量函数
SELECT id, name FROM goods WHERE LENGTH(name) >= 6;
SELECT id, name FROM goods WHERE UPPER(name) LIKE 'A%';

-- 标量函数与 NULLIF / COALESCE
SELECT id, COALESCE(NULLIF(on_sale, 0), 0) AS sale_flag FROM goods;

exit;