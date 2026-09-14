-- 37_insert_select.sql
-- INSERT ... SELECT：从一个 SELECT 的结果填充目标表。

CREATE TABLE src(id INT, name VARCHAR, score FLOAT);
CREATE TABLE dst(id INT, name VARCHAR, score FLOAT);

INSERT INTO src VALUES
    (1, 'Alice',  88.5),
    (2, 'Bob',    91.0),
    (3, 'Charlie',76.5),
    (4, 'David',  95.5),
    (5, 'Eve',    82.0);

INSERT INTO dst SELECT * FROM src;

SELECT COUNT(*) FROM dst;

INSERT INTO dst SELECT id, name, score FROM src WHERE score >= 90;

INSERT INTO dst SELECT id, UPPER(name), score + 1.0 FROM src ORDER BY id LIMIT 2;

INSERT INTO dst(id, name) SELECT id, name FROM src WHERE id <= 2;

SELECT * FROM dst ORDER BY id;

exit;
