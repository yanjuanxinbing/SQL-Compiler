-- 23_chinese_idents.sql
-- 反引号包裹的中文标识符（lexer 显式支持 BACKTICK）

CREATE TABLE `学生`(
    `编号` INT PRIMARY KEY,
    `姓名` VARCHAR(50),
    `成绩` FLOAT
);

INSERT INTO `学生` VALUES
    (1, '张三', 88.5),
    (2, '李四', 92.0),
    (3, '王五', 76.5);

SELECT * FROM `学生`;
SELECT `姓名`, `成绩` FROM `学生` WHERE `成绩` >= 80;

-- 中英混合标识符
CREATE TABLE `Order2026`(
    `id` INT,
    `客户` VARCHAR,
    `金额` FLOAT
);

INSERT INTO `Order2026` VALUES
    (1, 'Alice', 100.5),
    (2, 'Bob',   250.0),
    (3, 'Carol',  75.5);

-- JOIN 中英混合表
SELECT `学生`.`姓名`, `Order2026`.`金额`
FROM `学生`
INNER JOIN `Order2026` ON `学生`.`编号` = `Order2026`.`id`;

-- 中文字段上的聚合
SELECT COUNT(*) FROM `学生` WHERE `成绩` > 80;

-- 反引号别名（如果支持）
SELECT `编号` AS `id`, `姓名` AS `name` FROM `学生`;

exit;