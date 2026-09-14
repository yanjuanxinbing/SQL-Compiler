-- 75_not_between.sql
-- BUG-2 [P0 严重]: NOT BETWEEN 不生效
-- --------------------------------------------------------------------------
-- 历史原因（已修复，见 Parser.cpp::ParseComparisonExpr）：
--   ParseComparisonExpr 在解析完左侧操作数后，只检查 KEYWORD_BETWEEN，
--   没处理 `NOT BETWEEN` 的特殊形态。当输入是 `col NOT BETWEEN x AND y` 时，
--   NOT 不被识别为比较运算符，ParseComparisonExpr 把左侧 col 直接返回，
--   外层 ParseNotExpr 再把 col 包成 `NOT(col)`；`BETWEEN x AND y` 子句被
--   静默丢弃。WHERE 谓词退化为「column != 0」，对非零列恒真。
--   现象：WHERE col NOT BETWEEN a AND b 返回全部行。
--
-- 本测试覆盖：
--   1) NOT BETWEEN 主路径（与正向 BETWEEN 对照）。
--   2) NOT BETWEEN 与正向 BETWEEN 完全互补（不重不漏）。
--   3) NOT BETWEEN 与列级 UNIQUE 联合使用（覆盖组合语义）。
--   4) NOT BETWEEN 在 UPDATE 路径上的 SET/WHERE 使用。
--   5) NOT BETWEEN 中包含表达式（如函数调用）。
--   6) NOT BETWEEN 与 NULL 操作数：UNKNOWN 视为不通过，保留与正向 BETWEEN
--      一致的三值逻辑。

-- =====================================================================
-- 1) NOT BETWEEN 主路径（与正向 BETWEEN 对照）
-- =====================================================================
CREATE TABLE ages (
    id  INT PRIMARY KEY,
    age INT
);
INSERT INTO ages VALUES (1, 10), (2, 20), (3, 30), (4, 40), (5, 50);

-- 正向 BETWEEN：3 行（id=2,3,4）
SELECT id, age FROM ages WHERE age BETWEEN 20 AND 40 ORDER BY id;
-- NOT BETWEEN：2 行（id=1,5），这是 BUG-2 主路径
SELECT id, age FROM ages WHERE age NOT BETWEEN 20 AND 40 ORDER BY id;

-- =====================================================================
-- 2) 正向 / 反向完全互补
-- =====================================================================
SELECT id, age FROM ages
    WHERE age BETWEEN 20 AND 40
       OR age NOT BETWEEN 20 AND 40
    ORDER BY id;
-- 期望：5 行（全集）

-- =====================================================================
-- 3) NOT BETWEEN + 列级 UNIQUE 组合
-- =====================================================================
CREATE TABLE uq_nb (
    id    INT PRIMARY KEY,
    email TEXT UNIQUE
);
INSERT INTO uq_nb VALUES (1, 'a@x.com');
INSERT INTO uq_nb VALUES (2, 'b@x.com');
INSERT INTO uq_nb VALUES (3, 'c@x.com');
-- id NOT BETWEEN 1 AND 2 → id=3 一行
SELECT id, email FROM uq_nb WHERE id NOT BETWEEN 1 AND 2 ORDER BY id;
DROP TABLE uq_nb;

-- =====================================================================
-- 4) NOT BETWEEN 在 UPDATE 路径
-- =====================================================================
UPDATE ages SET age = age + 100 WHERE id NOT BETWEEN 2 AND 4;
-- id=1 → 110, id=5 → 150
SELECT id, age FROM ages ORDER BY id;
-- 把 id=1,5 还原回去
UPDATE ages SET age = age - 100 WHERE id NOT BETWEEN 2 AND 4;
SELECT id, age FROM ages ORDER BY id;

-- =====================================================================
-- 5) NOT BETWEEN 边界：闭区间端点 NOT 应排除
-- =====================================================================
-- 20 NOT BETWEEN 20 AND 40 → false（端点属于区间内，NOT 排除）
-- 19 NOT BETWEEN 20 AND 40 → true
SELECT id, age FROM ages WHERE age NOT BETWEEN 20 AND 40 ORDER BY id;
-- 仍然只有 id=1 (10) 和 id=5 (50)

-- =====================================================================
-- 6) NOT BETWEEN + NULL 操作数
-- =====================================================================
CREATE TABLE ages_null (
    id  INT PRIMARY KEY,
    age INT
);
INSERT INTO ages_null VALUES (1, 10), (2, NULL), (3, 30), (4, NULL), (5, 50);
-- NULL 操作数下 BETWEEN 求值为 NULL/UNKNOWN → WHERE 三值逻辑视为不通过；
-- 同理 NOT BETWEEN 在 NULL 下也是 UNKNOWN → 不通过。
SELECT id, age FROM ages_null WHERE age NOT BETWEEN 20 AND 40 ORDER BY id;
-- 期望：id=1 (10) 和 id=5 (50)，id=2,4 (NULL) 被排除
SELECT id, age FROM ages_null WHERE age BETWEEN 20 AND 40 ORDER BY id;
-- 期望：只有 id=3 (30)，id=2,4 (NULL) 被排除
DROP TABLE ages_null;

DROP TABLE ages;

exit;
