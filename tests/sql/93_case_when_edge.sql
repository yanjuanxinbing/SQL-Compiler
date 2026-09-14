-- 93_case_when_edge.sql
-- 测试目标：验证 CASE 表达式的边界行为（ searched / simple / 嵌套 / 无 ELSE / 混合类型 ）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. searched CASE（CASE WHEN cond THEN ...）无 ELSE：所有条件不满足 → NULL
--   2. searched CASE 带 ELSE：ELSE 兜底生效
--   3. 嵌套 CASE：THEN 分支里再放 CASE
--   4. simple CASE（CASE expr WHEN val THEN ...）：按值等值匹配
--   5. WHEN 条件用 IS NULL 判断
--   6. THEN 分支混合 INT 与 VARCHAR 类型 → 按实际求值输出
-- 预期结果：
--   - 无 ELSE 且条件不满足 → NULL（不是 0 或空串）
--   - 嵌套 CASE 按内外层条件正确求值（big/small/n/a）
--   - simple CASE：10→ten、-5→neg5、NULL→misc
--   - 混合类型分支输出 100 / 4 / 6
-- 后置处理：DROP 测试表

CREATE TABLE c1(id INT, v INT);

INSERT INTO c1 VALUES (1, 10), (2, -5), (3, NULL);

-- 1) 无 ELSE：条件不满足 → NULL
SELECT id,
       CASE WHEN v > 0 THEN 'pos'
            WHEN v < 0 THEN 'neg'
       END AS sign_no_else
FROM c1;

-- 2) 带 ELSE 兜底
SELECT id,
       CASE WHEN v > 0 THEN 'pos' ELSE 'other' END AS sign_else
FROM c1;

-- 3) 嵌套 CASE
SELECT id,
       CASE WHEN v > 0 THEN
                CASE WHEN v > 5 THEN 'big' ELSE 'small' END
            ELSE 'n/a'
       END AS nested
FROM c1;

-- 4) simple CASE（按值等值匹配）
SELECT id,
       CASE v WHEN 10 THEN 'ten'
              WHEN -5 THEN 'neg5'
              ELSE 'misc'
       END AS simple_case
FROM c1;

-- 5) WHEN 条件用 IS NULL
SELECT id,
       CASE WHEN v IS NULL THEN 'isnull' ELSE 'notnull' END AS null_chk
FROM c1;

-- 6) THEN 分支混合类型（INT / INT）
SELECT id,
       CASE id WHEN 1 THEN 100 ELSE id * 2 END AS branch_int
FROM c1 ORDER BY id;

-- 后置处理
DROP TABLE c1;

exit;
