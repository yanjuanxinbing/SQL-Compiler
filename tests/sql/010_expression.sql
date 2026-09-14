-- 11_expression.sql
-- 表达式：SELECT 中常量、字符串字面量、四则运算列

CREATE TABLE t(id INT, qty INT, price FLOAT, tax FLOAT);

INSERT INTO t VALUES
    (1, 3,  10.0, 0.5),
    (2, 5,  20.0, 1.0),
    (3, 2,   8.5, 0.4),
    (4, 10, 100.0, 5.0);

-- 常量列
SELECT 1, 3.14, 'hello' FROM t;

-- 列 + 常量
SELECT id, qty + 1 AS qty_plus_one FROM t;

-- 列 + 列
SELECT id, qty * price AS subtotal FROM t;

-- 复合算术：带括号优先级
SELECT id, (qty * price + tax) AS total FROM t;
SELECT id, qty * (price + tax) AS total2 FROM t;

-- 列 + 别名 + 重新引用
SELECT id, qty * price AS subtotal, subtotal + tax AS grand_total FROM t;

-- 负号 (UnaryOp)
SELECT id, -price AS neg_price FROM t;

-- WHERE 中的算术
SELECT * FROM t WHERE qty * price > 100;
SELECT * FROM t WHERE price + tax >= 20;

-- ORDER BY 中表达式（按计算列排序）
SELECT id, qty * price AS subtotal FROM t ORDER BY subtotal DESC;

exit;