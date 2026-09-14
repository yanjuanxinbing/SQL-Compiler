-- 61_typo_suggest.sql
-- 「Did you mean」建议功能测试：表名 / 列名拼写错误时，
-- SemanticAnalyzer 应在错误消息后追加距离 ≤ 2 的候选名。

CREATE TABLE student(id INT, name VARCHAR, age INT);
INSERT INTO student(id,name,age) VALUES (1,'Alice',20);
SELECT * FROM students;            -- typo: table (students vs student)
SELECT nmae FROM student;          -- typo: column (nmae vs name)
SELECT nmae, agee FROM student;    -- multiple typos
DROP TABLE student;
