-- 102_string_comparison.sql
-- 测试目标：验证 VARCHAR 比较语义（大小写敏感、字典序、空串边界）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. 大小写敏感性：'A'(65) < 'a'(97)，'Apple' 与 'apple' 不相等
--   2. 字典序：w < 'b' 涵盖 'Apple'/'apple'/'apple2'/''（均以 < b 的首字符起）
--   3. 范围比较：>= 'apple' AND <= 'applezz' → 前缀 'apple' 且小写开头者
--   4. 等值比较：'apple' 只匹配小写 apple
--   5. 空串是最小字符串：w > '' 排除空串行
-- 预期结果：
--   - w < 'b' → 1,3,4,5（apple / Apple / apple2 / ''）
--   - apple..applezz 范围 → 1,4（'Apple' 因 A<a 被排除）
--   - w = 'apple' → 1
--   - w > '' → 1,2,3,4（空串 5 被排除）
-- 后置处理：DROP 测试表

CREATE TABLE s1(id INT PRIMARY KEY, w VARCHAR);

INSERT INTO s1 VALUES
    (1, 'apple'),
    (2, 'banana'),
    (3, 'Apple'),
    (4, 'apple2'),
    (5, '');

-- 1) + 2) 大小写敏感的字典序比较
SELECT id FROM s1 WHERE w < 'b' ORDER BY id;

-- 3) 闭区间范围比较
SELECT id FROM s1 WHERE w >= 'apple' AND w <= 'applezz' ORDER BY id;

-- 4) 等值比较（大小写敏感）
SELECT id FROM s1 WHERE w = 'apple' ORDER BY id;

-- 5) 空串边界
SELECT id FROM s1 WHERE w > '' ORDER BY id;

-- 后置处理
DROP TABLE s1;

exit;
