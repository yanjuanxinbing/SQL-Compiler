-- 105_math_funcs_edge.sql
-- 测试目标：验证数学函数的边界行为（符号、负数、溢出、除零）
--
-- 前置条件：无（全新数据库）
-- 测试步骤：
--   1. ABS：正 / 负 / 零；INT_MIN 溢出回绕（C++ 行为，结果仍为 INT_MIN）
--   2. ROUND：四舍五入、负数远离零（ROUND(-3.5) = -4）
--   3. FLOOR / CEIL：负数方向
--   4. MOD（BUG-16 修复后语义）：余数符号跟随被除数（SQL 标准），
--      与二元运算符路径一致；除数为 0 → NULL
-- 预期结果：
--   - ABS(-5)=5, ABS(5)=5, ABS(0)=0
--   - ROUND(3.7)=4, ROUND(3.2)=3, ROUND(-3.5)=-4, ROUND(2.5)=3, ROUND(3.5)=4
--   - FLOOR(3.9)=3, FLOOR(-3.1)=-4, CEIL(3.1)=4, CEIL(-3.9)=-3
--   - MOD(10,3)=1, MOD(-10,3)=-1, MOD(10,-3)=1, MOD(-10,-3)=-1, MOD(10,0)=NULL
-- 后置处理：无表（仅 SELECT）

-- 1) ABS
SELECT ABS(-5) AS a1, ABS(5) AS a2, ABS(0) AS a3;

-- INT_MIN 取绝对值溢出回绕（记录 C++ 运行期行为）
SELECT ABS(-2147483648) AS int_min_abs;

-- 2) ROUND
SELECT ROUND(3.7) AS r1, ROUND(3.2) AS r2, ROUND(-3.5) AS r3;
SELECT ROUND(2.5) AS half1, ROUND(3.5) AS half2;

-- 3) FLOOR / CEIL
SELECT FLOOR(3.9) AS f1, FLOOR(-3.1) AS f2, CEIL(3.1) AS c1, CEIL(-3.9) AS c2;

-- 4) MOD：符号跟随被除数（BUG-16 修复：旧实现 MOD(10,-3)=-5、MOD(-10,-3)=-4）
SELECT MOD(10, 3)  AS m1,
       MOD(-10, 3) AS m2,
       MOD(10, -3) AS m3,
       MOD(-10, -3) AS m4;

-- 除零 → NULL
SELECT MOD(10, 0) AS m_zero;

exit;
