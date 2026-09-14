-- 02_dml.sql
-- DML 测试：INSERT（多行/指定列）、UPDATE、DELETE

CREATE TABLE emp(id INT, name VARCHAR, age INT, salary FLOAT);

-- 多行 INSERT
INSERT INTO emp VALUES
    (1, 'Alice', 30, 5000.0),
    (2, 'Bob', 25, 4000.0),
    (3, 'Charlie', 35, 6000.0),
    (4, 'David', 28, 4500.0);

-- 指定列 INSERT
INSERT INTO emp(name, age) VALUES ('Eve', 40);
INSERT INTO emp(name, salary) VALUES ('Frank', 3500.0);

SELECT * FROM emp;

-- UPDATE 测试
UPDATE emp SET salary = 5500.0 WHERE id = 1;
UPDATE emp SET age = 26, salary = 4200.0 WHERE name = 'Bob';
UPDATE emp SET salary = salary * 1.1 WHERE age > 30;

SELECT * FROM emp;

-- DELETE 测试
DELETE FROM emp WHERE age < 26;
DELETE FROM emp WHERE id = 3;

SELECT * FROM emp;

exit;
