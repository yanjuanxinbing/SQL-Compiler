-- ============================================================
-- 57_funcs: 函数 / 聚合扩展（Category 6）
-- ============================================================
-- 涵盖：
--   - 统计聚合：STDDEV / VARIANCE / MEDIAN
--     * 不带 _POP/_SAMP 后缀 = 样本（n-1 除数）；与 PostgreSQL 一致
--   - GROUPING SETS / ROLLUP / CUBE —— 多维子总计
--   - FILTER (WHERE cond) —— 聚合修饰
--   - PERCENTILE_CONT(p) WITHIN GROUP (ORDER BY x)
--   - PERCENTILE_DISC(p) WITHIN GROUP (ORDER BY x)
--   - STRING_AGG(expr, delim) —— 字符串聚合
--   - GREATEST / LEAST / RAND —— 标量函数
--   - RANGE BETWEEN 数值帧
--   - IGNORE NULLS（FIRST_VALUE 在帧内跳过 NULL）
-- ============================================================

-- ===========================================================
-- 1) STDDEV / VARIANCE / MEDIAN
-- ===========================================================
CREATE TABLE stats (v INT);
INSERT INTO stats VALUES (1), (2), (3), (4), (5);

-- 样本标准差（n-1 除数）：sqrt(2.5) ≈ 1.5811388
SELECT STDDEV(v), VARIANCE(v), MEDIAN(v) FROM stats;

-- 总体标准差 / 方差（n 除数）
SELECT STDDEV_POP(v), VAR_POP(v) FROM stats;

-- 单值：STDDEV/VARIANCE 返回 NULL（样本需要 n>=2）
CREATE TABLE one (v INT);
INSERT INTO one VALUES (42);
SELECT STDDEV(v), VARIANCE(v), MEDIAN(v) FROM one;

-- ===========================================================
-- 2) GROUPING SETS / ROLLUP / CUBE
-- ===========================================================
CREATE TABLE sales (region VARCHAR, product VARCHAR, amount INT);
INSERT INTO sales VALUES ('E','A',10),('E','B',20),('W','A',15),('W','B',25);

-- GROUPING SETS 三种维度：
--   (region)         → 2 行
--   (region, product)→ 4 行
--   ()               → 1 行（全总和）
SELECT region, product, SUM(amount) FROM sales GROUP BY GROUPING SETS ((region), (region, product), ());

-- ROLLUP(region, product) = GROUPING SETS ((region, product), (region), ())
SELECT region, product, SUM(amount) FROM sales GROUP BY ROLLUP(region, product);

-- CUBE(region, product) = 所有 4 个子集
SELECT region, product, SUM(amount) FROM sales GROUP BY CUBE(region, product);

-- ===========================================================
-- 3) FILTER (WHERE cond) —— 聚合修饰
-- ===========================================================
SELECT SUM(amount) FILTER (WHERE amount > 15) AS big_sales FROM sales;

-- ===========================================================
-- 4) WITHIN GROUP (ORDER BY ...) —— 有序集合聚合
-- ===========================================================
-- PERCENTILE_CONT(0.5) = 中位数（线性插值），这里 5 个值正好中位数 = 3
SELECT PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY v) AS median_cont FROM stats;
-- PERCENTILE_DISC(0.5) = 第 ceil(0.5*5)=3 行（1-based），即 v=3
SELECT PERCENTILE_DISC(0.5) WITHIN GROUP (ORDER BY v) AS median_disc FROM stats;

-- 边界分位：0 和 1 应分别返回最小值和最大值
SELECT PERCENTILE_CONT(0.0) WITHIN GROUP (ORDER BY v) AS p0,
       PERCENTILE_CONT(1.0) WITHIN GROUP (ORDER BY v) AS p1 FROM stats;

-- ===========================================================
-- 5) STRING_AGG(expr, delim)
-- ===========================================================
SELECT STRING_AGG(product, ',') FROM sales;

-- ===========================================================
-- 6) GREATEST / LEAST / RAND —— 标量函数
-- ===========================================================
SELECT GREATEST(1, 5, 3), LEAST(1, 5, 3);
-- RAND() 返回 [0, 1) 区间内的随机 double
SELECT RAND();
SELECT RANDOM();

-- ===========================================================
-- 7) RANGE BETWEEN 数值帧
-- ===========================================================
CREATE TABLE w (v INT, w_ INT);
INSERT INTO w VALUES (1,10), (2,20), (3,30), (4,40), (5,50);

-- RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING
--   v=1: {1, 2} SUM = 30
--   v=2: {1, 2, 3} SUM = 60
--   v=3: {2, 3, 4} SUM = 90
--   v=4: {3, 4, 5} SUM = 120
--   v=5: {4, 5} SUM = 90
SELECT v, SUM(w_) OVER (ORDER BY v RANGE BETWEEN 1 PRECEDING AND 1 FOLLOWING) FROM w ORDER BY v;

-- ===========================================================
-- 8) IGNORE NULLS —— 窗口函数跳过 NULL
-- ===========================================================
-- 语义：FIRST_VALUE(x IGNORE NULLS) OVER (...) 在帧内从 frame_start
-- 顺序扫到 frame_end，找首个非 NULL 的 x 返回。默认 frame =
-- RANGE BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW，因此：
--   v=1: frame={1} w_=NULL → 无非 NULL → NULL
--   v=2: frame={1,2} → 首个非 NULL = 20
--   v=3: frame={1,2,3} → 首个非 NULL = 20
--   v=4: frame={1,2,3,4} → 首个非 NULL = 20
--   v=5: frame={1,2,3,4,5} → 首个非 NULL = 20
CREATE TABLE n (v INT, w_ INT);
INSERT INTO n VALUES (1, NULL), (2, 20), (3, NULL), (4, 40), (5, 50);
SELECT v, FIRST_VALUE(w_ IGNORE NULLS) OVER (ORDER BY v) FROM n ORDER BY v;

-- 同样数据，把 frame 拉到整个分区（ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING），
-- 首个非 NULL 在整张表里都恒为 20：
SELECT v, FIRST_VALUE(w_ IGNORE NULLS) OVER (ORDER BY v ROWS BETWEEN UNBOUNDED PRECEDING AND UNBOUNDED FOLLOWING) FROM n ORDER BY v;

-- LAG IGNORE NULLS：沿 LAG 方向跳过 NULL。
-- v=1: lag 1 → 无前驱 → NULL
-- v=2: lag 1 → v=1 w_=NULL → 跳过，无更多 → NULL
-- v=3: lag 1 → v=2 w_=20 → 20
-- v=4: lag 1 → v=3 w_=NULL → 跳过 → v=2 w_=20 → 20
-- v=5: lag 1 → v=4 w_=40 → 40
SELECT v, LAG(w_ IGNORE NULLS) OVER (ORDER BY v) FROM n ORDER BY v;

exit;
