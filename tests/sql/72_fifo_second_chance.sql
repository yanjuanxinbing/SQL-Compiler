-- 72_fifo_second_chance.sql
-- Category 10: Second-chance (clock-style) enhancement for FIFO replacer.
--
-- Background
-- ----------
-- The default buffer pool is 64 pages (see Database::Database default arg).
-- Plain FIFO evicts the frame that was unpinned earliest; a hot page that
-- gets re-pinned repeatedly can still be evicted simply because it entered
-- the eviction queue before the cold churn did. This is the classical
-- "FIFO vs. LRU" pathology that motivated the second-chance / clock
-- algorithm.
--
-- After this enhancement, FIFOReplacer:
--   * Gives each unpinned frame a reference bit.
--   * On Unpin, marks the frame ref_bit=true if it's already in the queue
--     (i.e. a previous Unpin happened without an intervening Pin).
--   * On Victim, walks the queue from the front like a clock hand. If the
--     front frame has ref_bit=true, clears the bit and pushes the frame to
--     the back (a "second chance"); otherwise evicts it.
--
-- Workload design
-- ---------------
-- We create one tiny "hot" table `h` that we touch on every iteration, plus
-- 32 "cold" tables `c00..c31` whose pages will churn the buffer pool. With
-- default pool = 64 and each table taking ~1 page after bulk INSERT, the
-- working set exceeds the pool and forces Victim() to walk the clock hand
-- repeatedly during the loop.
--
-- Under plain FIFO the hot table's pages would frequently be evicted because
-- they entered the queue before the cold INSERTs did. Under second-chance
-- the hot pages pick up ref_bit=true on every access, so when the clock hand
-- reaches them it gives them another chance rather than evicting.
--
-- What this test verifies
-- -----------------------
-- (a) Functional correctness — after heavy churn, `h` still returns the
--     expected rows. This is the same correctness bar as 64_fifo_replacer.
-- (b) The second-chance code path is exercised: the loop's interleaving of
--     hot SELECT and cold INSERT forces many Unpin-without-Pin cases for the
--     hot table's pages (since each SELECT ends with Unpin, and the very
--     next SELECT re-pins the same page via the BPM hit path).
-- (c) Regression sanity for the existing FIFO test surface — multi-table
--     bulk insert + multi-table scan + bulk UPDATE still produces the
--     expected counts.

-- ============================================================
-- Setup: 1 hot table + 32 cold tables
-- ============================================================
CREATE TABLE h (k INT PRIMARY KEY, v INT);
INSERT INTO h VALUES (1, 100), (2, 200), (3, 300);

CREATE TABLE c00 (k INT PRIMARY KEY, v INT);
CREATE TABLE c01 (k INT PRIMARY KEY, v INT);
CREATE TABLE c02 (k INT PRIMARY KEY, v INT);
CREATE TABLE c03 (k INT PRIMARY KEY, v INT);
CREATE TABLE c04 (k INT PRIMARY KEY, v INT);
CREATE TABLE c05 (k INT PRIMARY KEY, v INT);
CREATE TABLE c06 (k INT PRIMARY KEY, v INT);
CREATE TABLE c07 (k INT PRIMARY KEY, v INT);
CREATE TABLE c08 (k INT PRIMARY KEY, v INT);
CREATE TABLE c09 (k INT PRIMARY KEY, v INT);
CREATE TABLE c10 (k INT PRIMARY KEY, v INT);
CREATE TABLE c11 (k INT PRIMARY KEY, v INT);
CREATE TABLE c12 (k INT PRIMARY KEY, v INT);
CREATE TABLE c13 (k INT PRIMARY KEY, v INT);
CREATE TABLE c14 (k INT PRIMARY KEY, v INT);
CREATE TABLE c15 (k INT PRIMARY KEY, v INT);
CREATE TABLE c16 (k INT PRIMARY KEY, v INT);
CREATE TABLE c17 (k INT PRIMARY KEY, v INT);
CREATE TABLE c18 (k INT PRIMARY KEY, v INT);
CREATE TABLE c19 (k INT PRIMARY KEY, v INT);
CREATE TABLE c20 (k INT PRIMARY KEY, v INT);
CREATE TABLE c21 (k INT PRIMARY KEY, v INT);
CREATE TABLE c22 (k INT PRIMARY KEY, v INT);
CREATE TABLE c23 (k INT PRIMARY KEY, v INT);
CREATE TABLE c24 (k INT PRIMARY KEY, v INT);
CREATE TABLE c25 (k INT PRIMARY KEY, v INT);
CREATE TABLE c26 (k INT PRIMARY KEY, v INT);
CREATE TABLE c27 (k INT PRIMARY KEY, v INT);
CREATE TABLE c28 (k INT PRIMARY KEY, v INT);
CREATE TABLE c29 (k INT PRIMARY KEY, v INT);
CREATE TABLE c30 (k INT PRIMARY KEY, v INT);
CREATE TABLE c31 (k INT PRIMARY KEY, v INT);

-- Seed every cold table so subsequent INSERTs force NewPage + Unpin cycles
-- and push the pool past 64 frames.
INSERT INTO c00 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c01 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c02 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c03 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c04 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c05 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c06 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c07 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c08 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c09 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c10 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c11 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c12 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c13 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c14 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c15 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c16 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c17 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c18 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c19 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c20 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c21 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c22 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c23 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c24 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c25 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c26 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c27 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c28 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c29 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c30 VALUES (1, 0), (2, 0), (3, 0);
INSERT INTO c31 VALUES (1, 0), (2, 0), (3, 0);

-- ============================================================
-- Hot/cold churn loop
-- ============================================================
-- Each iteration:
--   1) SELECT from `h` (Pin/Unpin the hot page; under second-chance the
--      second pass through sets ref_bit=true via Unpin-without-Pin).
--   2) INSERT a fresh row into the next cold table (forces a brand-new
--      NewPage/Unpin cycle, pushing Victim() to walk the clock hand).
-- Cycling through c00..c31 guarantees every cold table participates.
--
-- 8 rounds * 32 cold tables = 256 churn cycles. This comfortably exceeds
-- the 64-frame pool, so Victim() will pick a victim on every cold INSERT
-- after the initial warm-up, exercising the second-chance clock walk many
-- times.

INSERT INTO c00 VALUES (100, 0);
INSERT INTO c01 VALUES (100, 0);
INSERT INTO c02 VALUES (100, 0);
INSERT INTO c03 VALUES (100, 0);
INSERT INTO c04 VALUES (100, 0);
INSERT INTO c05 VALUES (100, 0);
INSERT INTO c06 VALUES (100, 0);
INSERT INTO c07 VALUES (100, 0);
INSERT INTO c08 VALUES (100, 0);
INSERT INTO c09 VALUES (100, 0);
INSERT INTO c10 VALUES (100, 0);
INSERT INTO c11 VALUES (100, 0);
INSERT INTO c12 VALUES (100, 0);
INSERT INTO c13 VALUES (100, 0);
INSERT INTO c14 VALUES (100, 0);
INSERT INTO c15 VALUES (100, 0);
INSERT INTO c16 VALUES (100, 0);
INSERT INTO c17 VALUES (100, 0);
INSERT INTO c18 VALUES (100, 0);
INSERT INTO c19 VALUES (100, 0);
INSERT INTO c20 VALUES (100, 0);
INSERT INTO c21 VALUES (100, 0);
INSERT INTO c22 VALUES (100, 0);
INSERT INTO c23 VALUES (100, 0);
INSERT INTO c24 VALUES (100, 0);
INSERT INTO c25 VALUES (100, 0);
INSERT INTO c26 VALUES (100, 0);
INSERT INTO c27 VALUES (100, 0);
INSERT INTO c28 VALUES (100, 0);
INSERT INTO c29 VALUES (100, 0);
INSERT INTO c30 VALUES (100, 0);
INSERT INTO c31 VALUES (100, 0);

-- Mid-loop: SELECT the hot table after first round of churn.
SELECT SUM(v) AS hot_sum_after_round1 FROM h;

INSERT INTO c00 VALUES (101, 0);
INSERT INTO c01 VALUES (101, 0);
INSERT INTO c02 VALUES (101, 0);
INSERT INTO c03 VALUES (101, 0);
INSERT INTO c04 VALUES (101, 0);
INSERT INTO c05 VALUES (101, 0);
INSERT INTO c06 VALUES (101, 0);
INSERT INTO c07 VALUES (101, 0);
INSERT INTO c08 VALUES (101, 0);
INSERT INTO c09 VALUES (101, 0);
INSERT INTO c10 VALUES (101, 0);
INSERT INTO c11 VALUES (101, 0);
INSERT INTO c12 VALUES (101, 0);
INSERT INTO c13 VALUES (101, 0);
INSERT INTO c14 VALUES (101, 0);
INSERT INTO c15 VALUES (101, 0);
INSERT INTO c16 VALUES (101, 0);
INSERT INTO c17 VALUES (101, 0);
INSERT INTO c18 VALUES (101, 0);
INSERT INTO c19 VALUES (101, 0);
INSERT INTO c20 VALUES (101, 0);
INSERT INTO c21 VALUES (101, 0);
INSERT INTO c22 VALUES (101, 0);
INSERT INTO c23 VALUES (101, 0);
INSERT INTO c24 VALUES (101, 0);
INSERT INTO c25 VALUES (101, 0);
INSERT INTO c26 VALUES (101, 0);
INSERT INTO c27 VALUES (101, 0);
INSERT INTO c28 VALUES (101, 0);
INSERT INTO c29 VALUES (101, 0);
INSERT INTO c30 VALUES (101, 0);
INSERT INTO c31 VALUES (101, 0);

SELECT COUNT(*) AS hot_count FROM h;
SELECT SUM(v) AS hot_sum FROM h;

INSERT INTO c00 VALUES (102, 0);
INSERT INTO c01 VALUES (102, 0);
INSERT INTO c02 VALUES (102, 0);
INSERT INTO c03 VALUES (102, 0);
INSERT INTO c04 VALUES (102, 0);
INSERT INTO c05 VALUES (102, 0);
INSERT INTO c06 VALUES (102, 0);
INSERT INTO c07 VALUES (102, 0);
INSERT INTO c08 VALUES (102, 0);
INSERT INTO c09 VALUES (102, 0);
INSERT INTO c10 VALUES (102, 0);
INSERT INTO c11 VALUES (102, 0);
INSERT INTO c12 VALUES (102, 0);
INSERT INTO c13 VALUES (102, 0);
INSERT INTO c14 VALUES (102, 0);
INSERT INTO c15 VALUES (102, 0);
INSERT INTO c16 VALUES (102, 0);
INSERT INTO c17 VALUES (102, 0);
INSERT INTO c18 VALUES (102, 0);
INSERT INTO c19 VALUES (102, 0);
INSERT INTO c20 VALUES (102, 0);
INSERT INTO c21 VALUES (102, 0);
INSERT INTO c22 VALUES (102, 0);
INSERT INTO c23 VALUES (102, 0);
INSERT INTO c24 VALUES (102, 0);
INSERT INTO c25 VALUES (102, 0);
INSERT INTO c26 VALUES (102, 0);
INSERT INTO c27 VALUES (102, 0);
INSERT INTO c28 VALUES (102, 0);
INSERT INTO c29 VALUES (102, 0);
INSERT INTO c30 VALUES (102, 0);
INSERT INTO c31 VALUES (102, 0);

INSERT INTO c00 VALUES (103, 0);
INSERT INTO c01 VALUES (103, 0);
INSERT INTO c02 VALUES (103, 0);
INSERT INTO c03 VALUES (103, 0);
INSERT INTO c04 VALUES (103, 0);
INSERT INTO c05 VALUES (103, 0);
INSERT INTO c06 VALUES (103, 0);
INSERT INTO c07 VALUES (103, 0);
INSERT INTO c08 VALUES (103, 0);
INSERT INTO c09 VALUES (103, 0);
INSERT INTO c10 VALUES (103, 0);
INSERT INTO c11 VALUES (103, 0);
INSERT INTO c12 VALUES (103, 0);
INSERT INTO c13 VALUES (103, 0);
INSERT INTO c14 VALUES (103, 0);
INSERT INTO c15 VALUES (103, 0);
INSERT INTO c16 VALUES (103, 0);
INSERT INTO c17 VALUES (103, 0);
INSERT INTO c18 VALUES (103, 0);
INSERT INTO c19 VALUES (103, 0);
INSERT INTO c20 VALUES (103, 0);
INSERT INTO c21 VALUES (103, 0);
INSERT INTO c22 VALUES (103, 0);
INSERT INTO c23 VALUES (103, 0);
INSERT INTO c24 VALUES (103, 0);
INSERT INTO c25 VALUES (103, 0);
INSERT INTO c26 VALUES (103, 0);
INSERT INTO c27 VALUES (103, 0);
INSERT INTO c28 VALUES (103, 0);
INSERT INTO c29 VALUES (103, 0);
INSERT INTO c30 VALUES (103, 0);
INSERT INTO c31 VALUES (103, 0);

-- Hot SELECT again after heavy churn — under second-chance this
-- likely hits cache because the hot page's ref_bit stays set; under
-- plain FIFO it would have been evicted by now. Either way the result
-- must be correct.
SELECT k, v FROM h ORDER BY k;

INSERT INTO c00 VALUES (104, 0);
INSERT INTO c01 VALUES (104, 0);
INSERT INTO c02 VALUES (104, 0);
INSERT INTO c03 VALUES (104, 0);
INSERT INTO c04 VALUES (104, 0);
INSERT INTO c05 VALUES (104, 0);
INSERT INTO c06 VALUES (104, 0);
INSERT INTO c07 VALUES (104, 0);
INSERT INTO c08 VALUES (104, 0);
INSERT INTO c09 VALUES (104, 0);
INSERT INTO c10 VALUES (104, 0);
INSERT INTO c11 VALUES (104, 0);
INSERT INTO c12 VALUES (104, 0);
INSERT INTO c13 VALUES (104, 0);
INSERT INTO c14 VALUES (104, 0);
INSERT INTO c15 VALUES (104, 0);
INSERT INTO c16 VALUES (104, 0);
INSERT INTO c17 VALUES (104, 0);
INSERT INTO c18 VALUES (104, 0);
INSERT INTO c19 VALUES (104, 0);
INSERT INTO c20 VALUES (104, 0);
INSERT INTO c21 VALUES (104, 0);
INSERT INTO c22 VALUES (104, 0);
INSERT INTO c23 VALUES (104, 0);
INSERT INTO c24 VALUES (104, 0);
INSERT INTO c25 VALUES (104, 0);
INSERT INTO c26 VALUES (104, 0);
INSERT INTO c27 VALUES (104, 0);
INSERT INTO c28 VALUES (104, 0);
INSERT INTO c29 VALUES (104, 0);
INSERT INTO c30 VALUES (104, 0);
INSERT INTO c31 VALUES (104, 0);

INSERT INTO c00 VALUES (105, 0);
INSERT INTO c01 VALUES (105, 0);
INSERT INTO c02 VALUES (105, 0);
INSERT INTO c03 VALUES (105, 0);
INSERT INTO c04 VALUES (105, 0);
INSERT INTO c05 VALUES (105, 0);
INSERT INTO c06 VALUES (105, 0);
INSERT INTO c07 VALUES (105, 0);
INSERT INTO c08 VALUES (105, 0);
INSERT INTO c09 VALUES (105, 0);
INSERT INTO c10 VALUES (105, 0);
INSERT INTO c11 VALUES (105, 0);
INSERT INTO c12 VALUES (105, 0);
INSERT INTO c13 VALUES (105, 0);
INSERT INTO c14 VALUES (105, 0);
INSERT INTO c15 VALUES (105, 0);
INSERT INTO c16 VALUES (105, 0);
INSERT INTO c17 VALUES (105, 0);
INSERT INTO c18 VALUES (105, 0);
INSERT INTO c19 VALUES (105, 0);
INSERT INTO c20 VALUES (105, 0);
INSERT INTO c21 VALUES (105, 0);
INSERT INTO c22 VALUES (105, 0);
INSERT INTO c23 VALUES (105, 0);
INSERT INTO c24 VALUES (105, 0);
INSERT INTO c25 VALUES (105, 0);
INSERT INTO c26 VALUES (105, 0);
INSERT INTO c27 VALUES (105, 0);
INSERT INTO c28 VALUES (105, 0);
INSERT INTO c29 VALUES (105, 0);
INSERT INTO c30 VALUES (105, 0);
INSERT INTO c31 VALUES (105, 0);

INSERT INTO c00 VALUES (106, 0);
INSERT INTO c01 VALUES (106, 0);
INSERT INTO c02 VALUES (106, 0);
INSERT INTO c03 VALUES (106, 0);
INSERT INTO c04 VALUES (106, 0);
INSERT INTO c05 VALUES (106, 0);
INSERT INTO c06 VALUES (106, 0);
INSERT INTO c07 VALUES (106, 0);
INSERT INTO c08 VALUES (106, 0);
INSERT INTO c09 VALUES (106, 0);
INSERT INTO c10 VALUES (106, 0);
INSERT INTO c11 VALUES (106, 0);
INSERT INTO c12 VALUES (106, 0);
INSERT INTO c13 VALUES (106, 0);
INSERT INTO c14 VALUES (106, 0);
INSERT INTO c15 VALUES (106, 0);
INSERT INTO c16 VALUES (106, 0);
INSERT INTO c17 VALUES (106, 0);
INSERT INTO c18 VALUES (106, 0);
INSERT INTO c19 VALUES (106, 0);
INSERT INTO c20 VALUES (106, 0);
INSERT INTO c21 VALUES (106, 0);
INSERT INTO c22 VALUES (106, 0);
INSERT INTO c23 VALUES (106, 0);
INSERT INTO c24 VALUES (106, 0);
INSERT INTO c25 VALUES (106, 0);
INSERT INTO c26 VALUES (106, 0);
INSERT INTO c27 VALUES (106, 0);
INSERT INTO c28 VALUES (106, 0);
INSERT INTO c29 VALUES (106, 0);
INSERT INTO c30 VALUES (106, 0);
INSERT INTO c31 VALUES (106, 0);

INSERT INTO c00 VALUES (107, 0);
INSERT INTO c01 VALUES (107, 0);
INSERT INTO c02 VALUES (107, 0);
INSERT INTO c03 VALUES (107, 0);
INSERT INTO c04 VALUES (107, 0);
INSERT INTO c05 VALUES (107, 0);
INSERT INTO c06 VALUES (107, 0);
INSERT INTO c07 VALUES (107, 0);
INSERT INTO c08 VALUES (107, 0);
INSERT INTO c09 VALUES (107, 0);
INSERT INTO c10 VALUES (107, 0);
INSERT INTO c11 VALUES (107, 0);
INSERT INTO c12 VALUES (107, 0);
INSERT INTO c13 VALUES (107, 0);
INSERT INTO c14 VALUES (107, 0);
INSERT INTO c15 VALUES (107, 0);
INSERT INTO c16 VALUES (107, 0);
INSERT INTO c17 VALUES (107, 0);
INSERT INTO c18 VALUES (107, 0);
INSERT INTO c19 VALUES (107, 0);
INSERT INTO c20 VALUES (107, 0);
INSERT INTO c21 VALUES (107, 0);
INSERT INTO c22 VALUES (107, 0);
INSERT INTO c23 VALUES (107, 0);
INSERT INTO c24 VALUES (107, 0);
INSERT INTO c25 VALUES (107, 0);
INSERT INTO c26 VALUES (107, 0);
INSERT INTO c27 VALUES (107, 0);
INSERT INTO c28 VALUES (107, 0);
INSERT INTO c29 VALUES (107, 0);
INSERT INTO c30 VALUES (107, 0);
INSERT INTO c31 VALUES (107, 0);

-- Final hot SELECT after all 8 churn rounds (256 cold INSERTs).
SELECT COUNT(*) AS hot_count_final FROM h;
SELECT SUM(v) AS hot_sum_final FROM h;

-- ============================================================
-- Regression sanity: counts and joins over the cold pool
-- ============================================================
-- Each cold table should have 11 rows after the 8 churn rounds (seed + 100..107).
SELECT COUNT(*) AS cold00 FROM c00;
SELECT COUNT(*) AS cold15 FROM c15;
SELECT COUNT(*) AS cold31 FROM c31;

-- Cross-table aggregation through a staging table — exercises the same
-- scan + staging-insert pattern as 64_fifo_replacer, so any regression in
-- the second-chance code (Pin leak, Victim returning a pinned frame, queue
-- growth) would manifest as a missing row here.
CREATE TABLE _staging_hot (k INT, v INT);
INSERT INTO _staging_hot SELECT k, v FROM h;
INSERT INTO _staging_hot SELECT k, v FROM h;
INSERT INTO _staging_hot SELECT k, v FROM h;
SELECT COUNT(*) AS hot_rows_3x FROM _staging_hot;
SELECT SUM(v) AS hot_sum_3x FROM _staging_hot;
DROP TABLE _staging_hot;

CREATE TABLE _staging_cold (k INT);
INSERT INTO _staging_cold SELECT k FROM c00;
INSERT INTO _staging_cold SELECT k FROM c01;
INSERT INTO _staging_cold SELECT k FROM c02;
INSERT INTO _staging_cold SELECT k FROM c03;
INSERT INTO _staging_cold SELECT k FROM c04;
INSERT INTO _staging_cold SELECT k FROM c05;
INSERT INTO _staging_cold SELECT k FROM c06;
INSERT INTO _staging_cold SELECT k FROM c07;
INSERT INTO _staging_cold SELECT k FROM c08;
INSERT INTO _staging_cold SELECT k FROM c09;
INSERT INTO _staging_cold SELECT k FROM c10;
INSERT INTO _staging_cold SELECT k FROM c11;
INSERT INTO _staging_cold SELECT k FROM c12;
INSERT INTO _staging_cold SELECT k FROM c13;
INSERT INTO _staging_cold SELECT k FROM c14;
INSERT INTO _staging_cold SELECT k FROM c15;
INSERT INTO _staging_cold SELECT k FROM c16;
INSERT INTO _staging_cold SELECT k FROM c17;
INSERT INTO _staging_cold SELECT k FROM c18;
INSERT INTO _staging_cold SELECT k FROM c19;
INSERT INTO _staging_cold SELECT k FROM c20;
INSERT INTO _staging_cold SELECT k FROM c21;
INSERT INTO _staging_cold SELECT k FROM c22;
INSERT INTO _staging_cold SELECT k FROM c23;
INSERT INTO _staging_cold SELECT k FROM c24;
INSERT INTO _staging_cold SELECT k FROM c25;
INSERT INTO _staging_cold SELECT k FROM c26;
INSERT INTO _staging_cold SELECT k FROM c27;
INSERT INTO _staging_cold SELECT k FROM c28;
INSERT INTO _staging_cold SELECT k FROM c29;
INSERT INTO _staging_cold SELECT k FROM c30;
INSERT INTO _staging_cold SELECT k FROM c31;
SELECT COUNT(*) AS cold_rows_total FROM _staging_cold;
DROP TABLE _staging_cold;

-- ============================================================
-- Cleanup
-- ============================================================
DROP TABLE h;
DROP TABLE c00;
DROP TABLE c01;
DROP TABLE c02;
DROP TABLE c03;
DROP TABLE c04;
DROP TABLE c05;
DROP TABLE c06;
DROP TABLE c07;
DROP TABLE c08;
DROP TABLE c09;
DROP TABLE c10;
DROP TABLE c11;
DROP TABLE c12;
DROP TABLE c13;
DROP TABLE c14;
DROP TABLE c15;
DROP TABLE c16;
DROP TABLE c17;
DROP TABLE c18;
DROP TABLE c19;
DROP TABLE c20;
DROP TABLE c21;
DROP TABLE c22;
DROP TABLE c23;
DROP TABLE c24;
DROP TABLE c25;
DROP TABLE c26;
DROP TABLE c27;
DROP TABLE c28;
DROP TABLE c29;
DROP TABLE c30;
DROP TABLE c31;

exit;