-- 19_multi_join.sql
-- 链式多表 JOIN：3 / 4 张表，混合 INNER/LEFT

CREATE TABLE student(id INT, name VARCHAR);
CREATE TABLE course(id INT, title VARCHAR);
CREATE TABLE enrollment(student_id INT, course_id INT, grade FLOAT);
CREATE TABLE teacher(id INT, name VARCHAR, course_id INT);

INSERT INTO student VALUES
    (1, 'Alice'),
    (2, 'Bob'),
    (3, 'Charlie'),
    (4, 'David');

INSERT INTO course VALUES
    (101, 'Math'),
    (102, 'Physics'),
    (103, 'Chemistry'),
    (104, 'Biology');

INSERT INTO enrollment VALUES
    (1, 101, 88.5),
    (1, 102, 92.0),
    (2, 101, 75.0),
    (3, 103, 95.0),
    (3, 104, 89.5),
    (4, 102, 80.0);

INSERT INTO teacher VALUES
    (201, 'Mr. Smith',  101),
    (202, 'Ms. Johnson',102),
    (203, 'Dr. Lee',    103);

-- 两表 INNER JOIN
SELECT s.name, c.title
FROM student s
INNER JOIN enrollment e ON s.id = e.student_id
INNER JOIN course    c ON e.course_id = c.id;

-- 三表 INNER JOIN（含 teacher）
SELECT s.name, c.title, t.name AS teacher
FROM student    s
INNER JOIN enrollment e ON s.id = e.student_id
INNER JOIN course    c ON e.course_id = c.id
INNER JOIN teacher   t ON c.id = t.course_id;

-- LEFT JOIN：没有老师的课程也保留
SELECT c.title, t.name AS teacher
FROM course    c
LEFT JOIN teacher t ON c.id = t.course_id;

-- LEFT JOIN：未选修任何课程的 student 也保留
SELECT s.name, c.title
FROM student s
LEFT JOIN enrollment e ON s.id = e.student_id
LEFT JOIN course     c ON e.course_id = c.id;

-- 多表 + 聚合：每门课的平均分
SELECT c.title, AVG(e.grade) AS avg_grade, COUNT(*) AS cnt
FROM student    s
INNER JOIN enrollment e ON s.id = e.student_id
INNER JOIN course    c ON e.course_id = c.id
GROUP BY c.title
ORDER BY avg_grade DESC;

exit;