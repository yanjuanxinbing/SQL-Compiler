-- 79_dml_subquery.sql
-- DELETE / UPDATE 的 WHERE 子句中带标量子查询的回归测试。
-- 修复前：DeleteExecutor 与 UpdateExecutor 用单参 ExpressionEvaluator(ctx_=nullptr)，
-- EvaluateSubquery 第一行 `if (!ctx_) return NULL` 触发短路，导致
-- `col = (SELECT ...)` 求值为 UNKNOWN，所有候选行被误判为不匹配。
-- 修复后：三个位置都补齐 —— 单表 UPDATE/DELETE 的三参构造 + Planner 预编译
-- SubqueryExprNode。

CREATE TABLE students(
    id INT,
    name VARCHAR,
    gpa FLOAT
);

CREATE TABLE enrollments(
    eid INT,
    sid INT,
    course VARCHAR
);

INSERT INTO students VALUES
    (1, 'Alice',   3.5),
    (2, 'Bob',     3.7),
    (3, 'Charlie', 3.95),
    (4, 'David',   3.2);

INSERT INTO enrollments VALUES
    (101, 1, 'CS101'),
    (102, 1, 'MA101'),
    (103, 2, 'CS101'),
    (104, 3, 'PH101'),
    (105, 4, 'BI101'),
    (106, 4, 'CH101');

-- DELETE 标量子查询：删除 David 的所有选课记录（预期 2 行）。
DELETE FROM enrollments WHERE sid = (SELECT id FROM students WHERE name = 'David');

-- 验证：David 的选课行确实没了；其他学生行未受影响。
SELECT * FROM enrollments ORDER BY eid;

-- UPDATE 标量子查询：把 Charlie 的 gpa 改成 4.0（预期 1 行）。
UPDATE students SET gpa = 4.0 WHERE id = (SELECT id FROM students WHERE name = 'Charlie');

-- 验证：Charlie 的 gpa 已是 4.0；其他人未受影响。
SELECT id, name, gpa FROM students ORDER BY id;

-- 子查询返回空集：应不删除任何行（= NULL ⇒ UNKNOWN ⇒ 全不匹配）。
DELETE FROM enrollments WHERE sid = (SELECT id FROM students WHERE name = 'Nobody');

-- 标量子查询 + 子查询在同一侧：嵌套子查询也能在 DELETE WHERE 中工作。
DELETE FROM enrollments
WHERE sid IN (SELECT id FROM students WHERE gpa >= 3.5);

-- 验证：gpa >= 3.5 的学生（Alice/Bob/Charlie）的所有选课记录已清空。
SELECT * FROM enrollments ORDER BY eid;

exit;
