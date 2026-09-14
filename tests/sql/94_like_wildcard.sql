-- 94_like_wildcard.sql
-- 测试目标：验证 LIKE 通配符匹配语义（% / _ / 大小写 / 转义行为）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. % 前缀 / 后缀 / 中置匹配
--   2. _ 单字符匹配（含定位匹配 '_____'）
--   3. 大小写敏感性：'Apple' 不匹配 'appl%'
--   4. 反斜杠转义：当前实现不把 \_ / \% 视为转义（按通配符/字符处理），
--      以实际行为记录
--   5. NOT LIKE 排除匹配
--   6. 无通配符的 LIKE = 精确等值
-- 预期结果（按探针实测行为记录）：
--   - LIKE 'appl%' → 1,4,6（apple / apple pie / appl_）
--   - LIKE '%pie' → 4
--   - LIKE '%a%' → 1,2,3,4,6（大小写敏感，'Apple'(5) 不含小写 a 于头部外仍算：
--     实测 Apple 不匹配，因为 Apple 中只有大写 A）
--   - LIKE 'appl_' → 1,6（_ 匹配任意单字符）
--   - LIKE '_____' → 1,3,5,6（恰好 5 字符）
--   - LIKE 'appl\_' → 1,6（\_ 未按转义处理，等价 appl_）
--   - LIKE '%' → 全部 7 行
--   - NOT LIKE 'appl%' → 2,3,5,7
--   - LIKE 'apple' → 1（无通配符精确匹配）
-- 后置处理：DROP 测试表

CREATE TABLE lk(id INT, s VARCHAR);

INSERT INTO lk VALUES
    (1, 'apple'),
    (2, 'banana'),
    (3, 'grape'),
    (4, 'apple pie'),
    (5, 'Apple'),
    (6, 'appl_'),
    (7, '100%');

-- 1) % 前缀匹配
SELECT id FROM lk WHERE s LIKE 'appl%';

-- % 后缀匹配
SELECT id FROM lk WHERE s LIKE '%pie';

-- % 双侧匹配
SELECT id FROM lk WHERE s LIKE '%a%';

-- 2) _ 单字符匹配
SELECT id FROM lk WHERE s LIKE 'appl_';

-- _ 定长匹配（恰好 5 个字符）
SELECT id FROM lk WHERE s LIKE '_____';

-- \_ 转义：当前实现按通配符 _ 处理（无转义语义）
SELECT id FROM lk WHERE s LIKE 'appl\_';

-- _ 中置匹配
SELECT id FROM lk WHERE s LIKE 'app_e';

-- 3) % 匹配一切
SELECT id FROM lk WHERE s LIKE '%';

-- 4) 字面量含 % 的数据用 \% 匹配
SELECT id FROM lk WHERE s LIKE '100\%';

-- 5) NOT LIKE
SELECT id FROM lk WHERE s NOT LIKE 'appl%';

-- 6) 无通配符 LIKE = 精确匹配
SELECT id FROM lk WHERE s LIKE 'apple';

-- 后置处理
DROP TABLE lk;

exit;
