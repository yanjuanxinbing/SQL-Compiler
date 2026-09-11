-- 38_join_variants.sql
-- JOIN 扩展：FULL OUTER JOIN / CROSS JOIN / USING (col) / NATURAL JOIN

CREATE TABLE a(id INT, name VARCHAR);
CREATE TABLE b(id INT, name VARCHAR);

INSERT INTO a VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol');
INSERT INTO b VALUES (1, 'Alice'), (2, 'David'), (4, 'Eve');

SELECT a.id, b.id FROM a INNER JOIN b ON a.id = b.id;

SELECT a.id, b.id FROM a FULL OUTER JOIN b ON a.id = b.id;

SELECT a.id, b.id FROM a CROSS JOIN b;

SELECT a.id FROM a INNER JOIN b USING (id);

SELECT a.id FROM a NATURAL JOIN b;

SELECT COUNT(*) FROM a INNER JOIN b ON a.id = b.id;

exit;
