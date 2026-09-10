-- 99_all_in_one.sql
-- 综合测试：DDL + DML + 各种查询

-- 建表
CREATE TABLE student(
    id INT PRIMARY KEY,
    name VARCHAR(50),
    age INT,
    dept_id INT,
    gpa FLOAT
);
CREATE TABLE department(id INT, name VARCHAR(50));
CREATE TABLE enrollment(student_id INT, course VARCHAR(50), grade FLOAT);

-- 初始数据
INSERT INTO department VALUES (1, 'CS'), (2, 'Math'), (3, 'Physics');
INSERT INTO student VALUES
    (1, 'Alice', 20, 1, 3.8),
    (2, 'Bob', 21, 1, 3.5),
    (3, 'Charlie', 22, 2, 3.9),
    (4, 'David', 20, 2, 3.2),
    (5, 'Eve', 23, 3, 3.7);

INSERT INTO enrollment VALUES
    (1, 'Algorithms', 92.0),
    (1, 'Database', 88.5),
    (2, 'Algorithms', 85.0),
    (3, 'Calculus', 95.0),
    (3, 'Linear Algebra', 90.0),
    (4, 'Calculus', 78.0),
    (5, 'Quantum', 91.5);

-- 查询 1：所有学生
SELECT '== 所有学生 ==' AS section;
SELECT * FROM student;

-- 查询 2：CS 系学生
SELECT '== CS 系学生 ==' AS section;
SELECT name, age, gpa FROM student WHERE dept_id = 1 ORDER BY gpa DESC;

-- 查询 3：JOIN
SELECT '== 学生-系 JOIN ==' AS section;
SELECT s.name, d.name AS dept, s.gpa
FROM student s
INNER JOIN department d ON s.dept_id = d.id
ORDER BY s.gpa DESC;

-- 查询 4：聚合
SELECT '== 各系人数与平均GPA ==' AS section;
SELECT d.name AS dept, COUNT(*) AS cnt, AVG(s.gpa) AS avg_gpa
FROM student s
INNER JOIN department d ON s.dept_id = d.id
GROUP BY d.name
HAVING COUNT(*) >= 1
ORDER BY avg_gpa DESC;

-- 查询 5：嵌套 + 多种操作符
SELECT '== 高 GPA 且选修了 Algorithms ==' AS section;
SELECT DISTINCT s.name, s.gpa
FROM student s
INNER JOIN enrollment e ON s.id = e.student_id
WHERE e.course = 'Algorithms' AND s.gpa > 3.5
ORDER BY s.gpa DESC
LIMIT 5;

-- 查询 6：IS NULL
SELECT '== NULL 演示 ==' AS section;
INSERT INTO enrollment VALUES (5, 'NULL_course_test', NULL);
SELECT * FROM enrollment WHERE grade IS NULL;
SELECT * FROM enrollment WHERE grade IS NOT NULL;

-- DML
SELECT '== UPDATE/DELETE 演示 ==' AS section;
UPDATE student SET gpa = gpa + 0.1 WHERE dept_id = 1;
SELECT name, gpa FROM student WHERE dept_id = 1;
DELETE FROM enrollment WHERE grade < 80;
SELECT * FROM enrollment;

exit;
