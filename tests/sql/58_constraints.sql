-- 58_constraints.sql
-- Category 7: 约束执行期语义补齐
--
-- 覆盖：
--   1) NOT NULL 在五种写入路径上的强制执行：
--      a) 单行 VALUES
--      b) 多行 VALUES
--      c) INSERT ... SELECT
--      d) UPDATE 把 NOT NULL 列置 NULL
--      e) INSERT ... ON DUPLICATE KEY UPDATE 把 NOT NULL 列置 NULL
--   2) 表级多列 CHECK (expr)
--      - 跨列引用 (e.g. price > cost)
--      - 单列引用 (e.g. c > 0)
--      - 同一张表挂多条 CHECK
--      - NULL 操作数按 SQL 标准三值逻辑"通过"
--   3) CONSTRAINT name CHECK (...) 命名约束
--      - 错误消息必须包含约束名（不只列名）

-- =====================================================================
-- 1) NOT NULL 跨全部写入路径
-- =====================================================================
CREATE TABLE nn1 (id INT PRIMARY KEY, x INT NOT NULL);

-- (a) 单行 VALUES：合法
INSERT INTO nn1 VALUES (1, 10);
-- (a) 显式 NULL：拒绝
INSERT INTO nn1 VALUES (2, NULL);

-- (b) 多行 VALUES：合法的两条 + 非法的一条
INSERT INTO nn1 VALUES (3, 30), (4, 40);
INSERT INTO nn1 VALUES (5, 50), (6, NULL), (7, 70);

-- (c) INSERT ... SELECT：候选里带 NULL → 拒绝
INSERT INTO nn1 SELECT 8, NULL FROM nn1 WHERE id = 1;
-- 合法版本
INSERT INTO nn1 SELECT 9, 90 FROM nn1 WHERE id = 1;
SELECT id, x FROM nn1 ORDER BY id;

-- (d) UPDATE 把 NOT NULL 列置 NULL → 拒绝
UPDATE nn1 SET x = NULL WHERE id = 1;
SELECT id, x FROM nn1 WHERE id = 1;

-- (e) INSERT ... ON DUPLICATE KEY UPDATE：SET 把它置 NULL → 拒绝
INSERT INTO nn1 VALUES (1, 100) ON DUPLICATE KEY UPDATE x = NULL;
SELECT id, x FROM nn1 WHERE id = 1;

-- =====================================================================
-- 2) 表级多列 CHECK
-- =====================================================================
CREATE TABLE orders (
    id INT PRIMARY KEY,
    price INT,
    cost INT,
    CHECK (price > cost)
);

-- 合法：100 > 50
INSERT INTO orders VALUES (1, 100, 50);
-- 非法：50 不 > 100
INSERT INTO orders VALUES (2, 50, 100);
-- NULL 操作数通过 (SQL 标准三值逻辑)：cost = NULL → CHECK 通过
INSERT INTO orders VALUES (3, 100, NULL);
-- UPDATE 改 cost 让 price <= cost → 拒绝
UPDATE orders SET cost = 200 WHERE id = 1;
SELECT id, price, cost FROM orders ORDER BY id;

-- =====================================================================
-- 3) 同一张表挂多条表级 CHECK
-- =====================================================================
CREATE TABLE multi (
    a INT,
    b INT,
    c INT,
    CHECK (a + b > c),
    CHECK (c > 0)
);

-- 5 + 6 = 11 > 10：合法
INSERT INTO multi VALUES (5, 6, 10);
-- 11 > 11 不成立 → 第一条 CHECK 拒绝
INSERT INTO multi VALUES (5, 6, 11);
-- c = -1：第二条 CHECK 拒绝
INSERT INTO multi VALUES (5, 6, -1);
-- NULL 操作数：a=NULL → (a+b > c) 求值为 NULL → 通过
-- 但 c = -1 仍让第二条拒绝
INSERT INTO multi VALUES (NULL, 6, -1);
SELECT a, b, c FROM multi ORDER BY c;

-- =====================================================================
-- 4) 命名约束：CONSTRAINT name CHECK (...)
-- =====================================================================
CREATE TABLE events (
    id INT PRIMARY KEY,
    start_d DATE,
    end_d DATE,
    CONSTRAINT dates_ok CHECK (start_d < end_d)
);

-- 合法
INSERT INTO events VALUES (1, DATE '2024-01-01', DATE '2024-12-31');
-- 非法：start_d 不 < end_d → 错误消息含 "dates_ok"
INSERT INTO events VALUES (2, DATE '2024-12-31', DATE '2024-01-01');
-- NULL 操作数通过
INSERT INTO events VALUES (3, NULL, DATE '2024-01-01');
SELECT id, start_d, end_d FROM events ORDER BY id;

exit;
