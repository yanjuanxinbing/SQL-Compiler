-- 116_min_max_string.sql
-- 测试目标：验证 MIN/MAX 聚合对 VARCHAR 的字典序比较（大小写敏感）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. MIN(w)/MAX(w) 对混合大小写字符串
--   2. 分组内的字符串 MIN/MAX
-- 预期结果：
--   - MIN('banana','apple','Apple','cherry') = 'Apple'（'A'(65) < 'a'(97)）
--   - MAX = 'cherry'（c > b > a，大小写敏感序）
--   - 分组：x 组 MIN='apple'（banana vs apple）
-- 后置处理：DROP 测试表

CREATE TABLE ms(id INT PRIMARY KEY, w VARCHAR);

INSERT INTO ms VALUES (1, 'banana'), (2, 'apple'), (3, 'Apple'), (4, 'cherry');

-- 1) 全表字符串 MIN/MAX
SELECT MIN(w) AS mn, MAX(w) AS mx FROM ms;

-- 2) 分组字符串 MIN/MAX
CREATE TABLE gms(id INT PRIMARY KEY, cat VARCHAR, w VARCHAR);

INSERT INTO gms VALUES (1, 'x', 'banana'), (2, 'x', 'apple'), (3, 'y', 'cherry');

SELECT cat, MIN(w) AS mn, MAX(w) AS mx FROM gms GROUP BY cat ORDER BY cat;

-- 后置处理
DROP TABLE ms;
DROP TABLE gms;

exit;
