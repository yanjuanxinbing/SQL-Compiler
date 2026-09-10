-- 18_arithmetic.sql
-- 运算符优先级 / 括号 / 比较运算边界

CREATE TABLE t(id INT, a INT, b INT, c FLOAT, d FLOAT);

INSERT INTO t VALUES
    (1, 10,  3,  1.5,  2.0),
    (2, 20,  5,  2.5,  0.5),
    (3, 30,  7,  3.5,  -1.0),
    (4, 40, 11,  0.0,  4.0);

-- 算术 + 比较
SELECT * FROM t WHERE a + b > 15;
SELECT * FROM t WHERE a - b < 10;
SELECT * FROM t WHERE a * b >= 100;
SELECT * FROM t WHERE a / b > 2;

-- 浮点比较
SELECT * FROM t WHERE c < d;
SELECT * FROM t WHERE c = 1.5;
SELECT * FROM t WHERE c != d;

-- 复合算术
SELECT id, a + b * 2 AS expr1, (a + b) * 2 AS expr2 FROM t;
SELECT id, a / b AS div1, a / (b * 1.0) AS div2 FROM t;

-- 除以 0 边界（行为取决于实现，通常返回 NULL）
SELECT id, a / 0 FROM t WHERE id = 1;
SELECT id, 0 / b FROM t;

-- 负数算术
SELECT id, -a AS neg_a FROM t;
SELECT id, -(a + b) AS neg_sum FROM t;

-- 综合：算术 + WHERE + ORDER BY
SELECT id, a + b AS total FROM t WHERE a + b > 10 ORDER BY total;

exit;