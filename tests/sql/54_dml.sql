-- ============================================================
-- 54_dml: DML 扩展（Category 3）
-- ============================================================
-- 涵盖：
--   - UPDATE ... FROM  (PG/Oracle 跨表更新)
--   - RETURNING on INSERT / UPDATE / DELETE
--   - MERGE INTO ... USING ... ON ... WHEN MATCHED / NOT MATCHED
--   - REPLACE INTO (MySQL 语义：先删后插)
-- ============================================================

-- ---------- 1. UPDATE ... FROM ----------
CREATE TABLE products (id INT PRIMARY KEY, price INT);
CREATE TABLE sales (product_id INT, sold INT);
INSERT INTO products VALUES (1, 100), (2, 200), (3, 300);
INSERT INTO sales VALUES (1, 5), (2, 3), (3, 8);
UPDATE products SET price = price + sales.sold * 10
    FROM sales WHERE products.id = sales.product_id;
SELECT * FROM products ORDER BY id;

-- ---------- 2. RETURNING on UPDATE ----------
CREATE TABLE r1 (id INT PRIMARY KEY, val INT);
INSERT INTO r1 VALUES (1, 10), (2, 20);
UPDATE r1 SET val = val + 1 WHERE id = 1 RETURNING id, val AS new_val;

-- ---------- 3. RETURNING on DELETE ----------
DELETE FROM r1 WHERE id = 2 RETURNING id, val;

-- ---------- 4. RETURNING on INSERT ----------
INSERT INTO r1 VALUES (3, 30) RETURNING id, val;

-- ---------- 5. MERGE ----------
CREATE TABLE target (id INT PRIMARY KEY, val INT);
CREATE TABLE source (id INT, val INT);
INSERT INTO target VALUES (1, 100), (2, 200);
INSERT INTO source VALUES (1, 999), (3, 300);
MERGE INTO target t USING source s ON t.id = s.id
    WHEN MATCHED THEN UPDATE SET val = s.val
    WHEN NOT MATCHED THEN INSERT (id, val) VALUES (s.id, s.val);
SELECT * FROM target ORDER BY id;

-- ---------- 6. REPLACE INTO ----------
CREATE TABLE rk (id INT PRIMARY KEY, val INT);
INSERT INTO rk VALUES (1, 100);
REPLACE INTO rk VALUES (1, 999);
SELECT * FROM rk;
REPLACE INTO rk VALUES (2, 200);
SELECT * FROM rk ORDER BY id;
