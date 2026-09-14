-- 107_coalesce_nullif.sql
-- 测试目标：验证 NULL 处理函数 COALESCE / IFNULL / NULLIF 的语义与组合
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. COALESCE 多参数：返回第一个非 NULL
--   2. IFNULL 两参数：NULL → 替换值；非 NULL → 原值
--   3. NULLIF(a,b)：a=b → NULL；a≠b → a
--   4. 组合：COALESCE(NULLIF(x, 10), 99) —— x=10 时取 99
--   5. 列上应用：COALESCE(v, 0) / IFNULL(v, -1) 替换 NULL 列值
-- 预期结果：
--   - COALESCE(NULL,NULL,3)=3、COALESCE(NULL,'x')='x'
--   - IFNULL(NULL,5)=5、IFNULL(7,5)=7
--   - NULLIF(1,1)=NULL、NULLIF(1,2)=1
--   - combo=99
--   - 表 cn：id=1(v=NULL) → v0=0, vn=-1；id=2(v=8) → v0=8, vn=8
-- 后置处理：DROP 测试表

-- 1) COALESCE 多参数
SELECT COALESCE(NULL, NULL, 3) AS co1;
SELECT COALESCE(NULL, 'x') AS co2;

-- 2) IFNULL
SELECT IFNULL(NULL, 5) AS if1, IFNULL(7, 5) AS if2;

-- 3) NULLIF
SELECT NULLIF(1, 1) AS n1, NULLIF(1, 2) AS n2;

-- 4) 组合
SELECT COALESCE(NULLIF(10, 10), 99) AS combo;

-- 5) 列上应用
CREATE TABLE cn(id INT PRIMARY KEY, v INT);

INSERT INTO cn VALUES (1, NULL), (2, 8);

SELECT id, COALESCE(v, 0) AS v0, IFNULL(v, -1) AS vn FROM cn ORDER BY id;

-- 后置处理
DROP TABLE cn;

exit;
