-- 90_typeof_function.sql
-- typeof(expr)：返回表达式求值结果的运行期类型名（VARCHAR）。
-- 覆盖：字面量 / 列 / 表达式 / NULL 值 / WHERE 与 CASE WHEN 中的用法。

-- 1) typeof 对字面量的基础行为
SELECT typeof(1)         AS t_int,
       typeof(1.5)       AS t_float,
       typeof('hi')      AS t_str,
       typeof(NULL)      AS t_null,
       typeof(1 + 2)     AS t_arith,
       typeof('a' || 'b') AS t_concat,
       typeof(UPPER('x')) AS t_call;

-- 2) typeof 对表达式结果（保证先求值后取类型）
SELECT typeof(1 + 2 * 3)     AS arith_int,
       typeof(1.0 + 2)       AS arith_float,
       typeof(CAST(1 AS FLOAT)) AS cast_float,
       typeof(CAST('abc' AS INT)) AS cast_int_or_null;

-- 3) typeof 对列（需要 CREATE TABLE）
CREATE TABLE t_typeof(
    id INT,
    name VARCHAR,
    price FLOAT,
    note VARCHAR
);

INSERT INTO t_typeof VALUES
    (1, 'apple',  3.14, NULL),
    (2, 'banana', 2.71, 'yellow'),
    (3, 'cherry', 1.41, NULL);

SELECT id, name,
       typeof(id)    AS col_id_type,
       typeof(name)  AS col_name_type,
       typeof(price) AS col_price_type,
       typeof(note)  AS col_note_type
FROM t_typeof;

-- 4) typeof 对 NULL 列值仍然返回其列的运行期类型（不会传播 NULL）
SELECT typeof(note) AS note_type_with_null_value FROM t_typeof WHERE note IS NULL;

-- 5) typeof 在 WHERE 子句中：作为普通值参与谓词
SELECT id, name FROM t_typeof WHERE typeof(name) = 'VARCHAR';
SELECT id, name FROM t_typeof WHERE typeof(price) = 'FLOAT';

-- 6) typeof 在 CASE WHEN 中：作为 THEN 分支的值
SELECT id,
       CASE
           WHEN typeof(name) = 'VARCHAR' THEN 'string col'
           WHEN typeof(id)   = 'INTEGER' THEN 'int col'
           ELSE 'other'
       END AS kind
FROM t_typeof;

-- 7) typeof 对 UPDATE 后的值（确认与运行期一致）
UPDATE t_typeof SET price = 9.99 WHERE id = 1;
SELECT typeof(price) AS updated_price_type FROM t_typeof WHERE id = 1;

exit;
