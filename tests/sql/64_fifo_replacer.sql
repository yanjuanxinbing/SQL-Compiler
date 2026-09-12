-- 64_fifo_replacer.sql
-- Category 10: Buffer pool churn regression for FIFO replacer
--
-- Coverage:
--   * Create many small tables to exceed the default 64-page buffer pool.
--   * Bulk INSERT / UPDATE / DELETE to force frequent Pin/Unpin cycles.
--   * Multi-way JOIN + aggregation to exercise both scan and probe access.
--   * Mixed SELECT after churn to confirm FIFO order still picks a valid
--     frame (i.e. stale Pin entries no longer get returned as victims).
--
-- This is a smoke test: we cannot directly observe memory usage from a
-- SQL script, but if the FIFO logic regresses (e.g. Pin leaks, Victim
-- returns a pinned frame, or the queue grows without bound and a frame
-- is evicted while still pinned), the SELECTs below will report missing
-- rows or "Error:" lines.

-- ============================================================
-- Part A: create N small tables (enough pages to exceed 64-frame pool)
-- ============================================================
CREATE TABLE a1 (k INT PRIMARY KEY, v INT);
CREATE TABLE a2 (k INT PRIMARY KEY, v INT);
CREATE TABLE a3 (k INT PRIMARY KEY, v INT);
CREATE TABLE a4 (k INT PRIMARY KEY, v INT);
CREATE TABLE a5 (k INT PRIMARY KEY, v INT);
CREATE TABLE a6 (k INT PRIMARY KEY, v INT);
CREATE TABLE a7 (k INT PRIMARY KEY, v INT);
CREATE TABLE a8 (k INT PRIMARY KEY, v INT);
CREATE TABLE a9 (k INT PRIMARY KEY, v INT);
CREATE TABLE a10 (k INT PRIMARY KEY, v INT);
CREATE TABLE a11 (k INT PRIMARY KEY, v INT);
CREATE TABLE a12 (k INT PRIMARY KEY, v INT);
CREATE TABLE a13 (k INT PRIMARY KEY, v INT);
CREATE TABLE a14 (k INT PRIMARY KEY, v INT);
CREATE TABLE a15 (k INT PRIMARY KEY, v INT);
CREATE TABLE a16 (k INT PRIMARY KEY, v INT);

-- ============================================================
-- Part B: bulk INSERT across all 16 tables (forces many Pin/Unpin cycles)
-- ============================================================
INSERT INTO a1  VALUES (1, 10), (2, 20), (3, 30);
INSERT INTO a2  VALUES (1, 11), (2, 21), (3, 31);
INSERT INTO a3  VALUES (1, 12), (2, 22), (3, 32);
INSERT INTO a4  VALUES (1, 13), (2, 23), (3, 33);
INSERT INTO a5  VALUES (1, 14), (2, 24), (3, 34);
INSERT INTO a6  VALUES (1, 15), (2, 25), (3, 35);
INSERT INTO a7  VALUES (1, 16), (2, 26), (3, 36);
INSERT INTO a8  VALUES (1, 17), (2, 27), (3, 37);
INSERT INTO a9  VALUES (1, 18), (2, 28), (3, 38);
INSERT INTO a10 VALUES (1, 19), (2, 29), (3, 39);
INSERT INTO a11 VALUES (1, 110), (2, 120), (3, 130);
INSERT INTO a12 VALUES (1, 111), (2, 121), (3, 131);
INSERT INTO a13 VALUES (1, 112), (2, 122), (3, 132);
INSERT INTO a14 VALUES (1, 113), (2, 123), (3, 133);
INSERT INTO a15 VALUES (1, 114), (2, 124), (3, 134);
INSERT INTO a16 VALUES (1, 115), (2, 125), (3, 135);

-- ============================================================
-- Part C: scan + join churn. Each query touches many tables so the
-- replacer must evict pages between statements. We use a staging
-- table for cross-table aggregation since this parser does not yet
-- support derived-table aliases.
-- ============================================================
CREATE TABLE _staging (k INT, v INT);
INSERT INTO _staging SELECT k, v FROM a1;
INSERT INTO _staging SELECT k, v FROM a2;
INSERT INTO _staging SELECT k, v FROM a3;
INSERT INTO _staging SELECT k, v FROM a4;
INSERT INTO _staging SELECT k, v FROM a5;
INSERT INTO _staging SELECT k, v FROM a6;
INSERT INTO _staging SELECT k, v FROM a7;
INSERT INTO _staging SELECT k, v FROM a8;
INSERT INTO _staging SELECT k, v FROM a9;
INSERT INTO _staging SELECT k, v FROM a10;
INSERT INTO _staging SELECT k, v FROM a11;
INSERT INTO _staging SELECT k, v FROM a12;
INSERT INTO _staging SELECT k, v FROM a13;
INSERT INTO _staging SELECT k, v FROM a14;
INSERT INTO _staging SELECT k, v FROM a15;
INSERT INTO _staging SELECT k, v FROM a16;
SELECT COUNT(*) AS total_rows FROM _staging;
SELECT SUM(v) AS grand_total FROM _staging;
DROP TABLE _staging;

-- Multi-table join probe — exercises both seq-scan and join probe reads.
SELECT a1.k AS k1, a2.v AS v2, a3.v AS v3
FROM a1
JOIN a2 ON a1.k = a2.k
JOIN a3 ON a1.k = a3.k
ORDER BY a1.k;

-- ============================================================
-- Part D: UPDATE churn — every row in every table, multiple times
-- ============================================================
UPDATE a1  SET v = v + 1000;
UPDATE a2  SET v = v + 1000;
UPDATE a3  SET v = v + 1000;
UPDATE a4  SET v = v + 1000;
UPDATE a5  SET v = v + 1000;
UPDATE a6  SET v = v + 1000;
UPDATE a7  SET v = v + 1000;
UPDATE a8  SET v = v + 1000;
UPDATE a9  SET v = v + 1000;
UPDATE a10 SET v = v + 1000;
UPDATE a11 SET v = v + 1000;
UPDATE a12 SET v = v + 1000;
UPDATE a13 SET v = v + 1000;
UPDATE a14 SET v = v + 1000;
UPDATE a15 SET v = v + 1000;
UPDATE a16 SET v = v + 1000;

UPDATE a1  SET v = v + 1000;
UPDATE a2  SET v = v + 1000;
UPDATE a3  SET v = v + 1000;
UPDATE a4  SET v = v + 1000;
UPDATE a5  SET v = v + 1000;
UPDATE a6  SET v = v + 1000;
UPDATE a7  SET v = v + 1000;
UPDATE a8  SET v = v + 1000;

-- ============================================================
-- Part E: Verify final state — every table should still have 3 rows.
-- ============================================================
SELECT COUNT(*) AS cnt_a1  FROM a1;
SELECT COUNT(*) AS cnt_a2  FROM a2;
SELECT COUNT(*) AS cnt_a3  FROM a3;
SELECT COUNT(*) AS cnt_a4  FROM a4;
SELECT COUNT(*) AS cnt_a5  FROM a5;
SELECT COUNT(*) AS cnt_a6  FROM a6;
SELECT COUNT(*) AS cnt_a7  FROM a7;
SELECT COUNT(*) AS cnt_a8  FROM a8;
SELECT COUNT(*) AS cnt_a9  FROM a9;
SELECT COUNT(*) AS cnt_a10 FROM a10;
SELECT COUNT(*) AS cnt_a11 FROM a11;
SELECT COUNT(*) AS cnt_a12 FROM a12;
SELECT COUNT(*) AS cnt_a13 FROM a13;
SELECT COUNT(*) AS cnt_a14 FROM a14;
SELECT COUNT(*) AS cnt_a15 FROM a15;
SELECT COUNT(*) AS cnt_a16 FROM a16;

-- ============================================================
-- Part F: DELETE churn — drop every other row, re-verify.
-- ============================================================
DELETE FROM a1  WHERE k = 2;
DELETE FROM a2  WHERE k = 2;
DELETE FROM a3  WHERE k = 2;
DELETE FROM a4  WHERE k = 2;
DELETE FROM a5  WHERE k = 2;
DELETE FROM a6  WHERE k = 2;
DELETE FROM a7  WHERE k = 2;
DELETE FROM a8  WHERE k = 2;
DELETE FROM a9  WHERE k = 2;
DELETE FROM a10 WHERE k = 2;
DELETE FROM a11 WHERE k = 2;
DELETE FROM a12 WHERE k = 2;
DELETE FROM a13 WHERE k = 2;
DELETE FROM a14 WHERE k = 2;
DELETE FROM a15 WHERE k = 2;
DELETE FROM a16 WHERE k = 2;

-- Each table should now have exactly 2 rows.
SELECT COUNT(*) AS cnt2 FROM a1;
SELECT COUNT(*) AS cnt2 FROM a8;
SELECT COUNT(*) AS cnt2 FROM a16;

-- Final integrity check across all tables (32 rows total).
CREATE TABLE _staging2 (k INT);
INSERT INTO _staging2 SELECT k FROM a1;
INSERT INTO _staging2 SELECT k FROM a2;
INSERT INTO _staging2 SELECT k FROM a3;
INSERT INTO _staging2 SELECT k FROM a4;
INSERT INTO _staging2 SELECT k FROM a5;
INSERT INTO _staging2 SELECT k FROM a6;
INSERT INTO _staging2 SELECT k FROM a7;
INSERT INTO _staging2 SELECT k FROM a8;
INSERT INTO _staging2 SELECT k FROM a9;
INSERT INTO _staging2 SELECT k FROM a10;
INSERT INTO _staging2 SELECT k FROM a11;
INSERT INTO _staging2 SELECT k FROM a12;
INSERT INTO _staging2 SELECT k FROM a13;
INSERT INTO _staging2 SELECT k FROM a14;
INSERT INTO _staging2 SELECT k FROM a15;
INSERT INTO _staging2 SELECT k FROM a16;
SELECT COUNT(*) AS total_after_delete FROM _staging2;
DROP TABLE _staging2;

-- ============================================================
-- Part G: cleanup
-- ============================================================
DROP TABLE a1;
DROP TABLE a2;
DROP TABLE a3;
DROP TABLE a4;
DROP TABLE a5;
DROP TABLE a6;
DROP TABLE a7;
DROP TABLE a8;
DROP TABLE a9;
DROP TABLE a10;
DROP TABLE a11;
DROP TABLE a12;
DROP TABLE a13;
DROP TABLE a14;
DROP TABLE a15;
DROP TABLE a16;

exit;
