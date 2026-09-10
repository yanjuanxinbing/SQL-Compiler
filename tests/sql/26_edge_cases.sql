-- 09_edge_cases.sql
-- 边界条件 / 异常处理（优雅路径）/ 性能冒烟测试
--
-- 说明：本测试套件（run_all_tests.bat）将任何输出 "Error:" 行的语句判定为失败，
--       因此"预期报错"类语句（表不存在、语法错误、类型不支持等）不纳入本自动化文件，
--       该类路径由单元测试（tests/*_test.cpp）覆盖；
--       本文件覆盖引擎内部的优雅异常处理路径：
--       除零→NULL、空结果集、0行UPDATE/DELETE、NULL传播、LIMIT边界等。

-- ============ 1. 数值边界 ============
CREATE TABLE num_edge(id INT, v INT, f FLOAT);
INSERT INTO num_edge VALUES (1, 2147483647, 1e15);   -- INT最大值 + 科学记数法(正指数,无点无符号)
INSERT INTO num_edge VALUES (2, -2147483647, 2E3);   -- INT负边界 + 大写E指数
INSERT INTO num_edge VALUES (3, 0, 1.5e-3);          -- 零 + 带符号负指数
INSERT INTO num_edge VALUES (4, -0, -0.0);           -- 负零
INSERT INTO num_edge VALUES (5, 100, -2.5e-3);
SELECT * FROM num_edge;
SELECT id, v FROM num_edge WHERE v = 0 OR v = 2147483647;
SELECT id, f * 2 AS doubled, f + 1 AS shifted FROM num_edge WHERE id <= 3;

-- ============ 2. 字符串边界 ============
CREATE TABLE str_edge(id INT, s VARCHAR(100));
INSERT INTO str_edge VALUES (1, '');
INSERT INTO str_edge VALUES (2, 'x');
INSERT INTO str_edge VALUES (3, 'tab\there and ''quoted''');
INSERT INTO str_edge VALUES (4, '中文English混排 0123456789 & symbols: yes!');
INSERT INTO str_edge VALUES (5, 'aaaaaaaaaabbbbbbbbbbccccccccccddddddddddeeeeeeeeeeffffffffffgggggggggghhhhhhhhhh');
SELECT * FROM str_edge;
SELECT id FROM str_edge WHERE s = '';
SELECT id FROM str_edge WHERE s LIKE '%中文%';
SELECT id FROM str_edge WHERE s LIKE 'a%';
SELECT id FROM str_edge WHERE s LIKE '%gg';

-- ============ 3. NULL 与空集 ============
CREATE TABLE null_edge(id INT, a INT, b VARCHAR, c FLOAT);
INSERT INTO null_edge VALUES (1, 10, 'x', 1.5);
INSERT INTO null_edge VALUES (2, NULL, NULL, NULL);
INSERT INTO null_edge VALUES (3, 30, 'z', 3.5);
INSERT INTO null_edge(id, a) VALUES (4, 40);          -- 部分列插入，未指定列为NULL
SELECT * FROM null_edge;
SELECT id FROM null_edge WHERE a IS NULL;
SELECT id FROM null_edge WHERE b IS NOT NULL;
SELECT COUNT(*), COUNT(a), COUNT(b), COUNT(c) FROM null_edge;
SELECT SUM(a), AVG(a), MIN(a), MAX(a) FROM null_edge;
SELECT COUNT(*) FROM null_edge WHERE 1 = 2;           -- 空集上的聚合
SELECT * FROM null_edge WHERE id = 999;               -- 空结果集
SELECT id FROM null_edge ORDER BY a;                  -- NULL参与排序
SELECT id FROM null_edge ORDER BY a DESC LIMIT 2;
SELECT id FROM null_edge LIMIT 0;                     -- LIMIT下界
SELECT id FROM null_edge LIMIT 100;                   -- LIMIT超过行数
SELECT DISTINCT a FROM null_edge;                     -- DISTINCT含NULL

-- ============ 4. 异常处理优雅路径 ============
CREATE TABLE safe_op(id INT, x INT, y FLOAT);
INSERT INTO safe_op VALUES (1, 10, 2.5);
INSERT INTO safe_op VALUES (2, -6, 0.0);
INSERT INTO safe_op VALUES (3, 7, -1.25);
SELECT id, x / 0 AS dz_int, y / 0.0 AS dz_float FROM safe_op;   -- 除零→NULL
SELECT id FROM safe_op WHERE x BETWEEN 100 AND 1;               -- 反向区间→空集
UPDATE safe_op SET x = 0 WHERE id = 999;                        -- 0行更新
DELETE FROM safe_op WHERE id = 999;                             -- 0行删除
SELECT COUNT(*) FROM safe_op;                                   -- 数据应不受影响（3行）

-- ============ 5. 性能冒烟：150行跨多页(4KB/页) + 批量聚合 ============
CREATE TABLE perf(id INT, cat VARCHAR(8), score FLOAT, val INT);
INSERT INTO perf VALUES
    (1, 'A', 0.5, 1), (2, 'B', 1.0, 2), (3, 'C', 1.5, 3), (4, 'A', 2.0, 4), (5, 'B', 2.5, 5),
    (6, 'C', 3.0, 6), (7, 'A', 3.5, 7), (8, 'B', 4.0, 8), (9, 'C', 4.5, 9), (10, 'A', 5.0, 0),
    (11, 'B', 5.5, 1), (12, 'C', 6.0, 2), (13, 'A', 6.5, 3), (14, 'B', 7.0, 4), (15, 'C', 7.5, 5),
    (16, 'A', 8.0, 6), (17, 'B', 8.5, 7), (18, 'C', 9.0, 8), (19, 'A', 9.5, 9), (20, 'B', 10.0, 0),
    (21, 'C', 10.5, 1), (22, 'A', 11.0, 2), (23, 'B', 11.5, 3), (24, 'C', 12.0, 4), (25, 'A', 12.5, 5),
    (26, 'B', 13.0, 6), (27, 'C', 13.5, 7), (28, 'A', 14.0, 8), (29, 'B', 14.5, 9), (30, 'C', 15.0, 0),
    (31, 'A', 15.5, 1), (32, 'B', 16.0, 2), (33, 'C', 16.5, 3), (34, 'A', 17.0, 4), (35, 'B', 17.5, 5),
    (36, 'C', 18.0, 6), (37, 'A', 18.5, 7), (38, 'B', 19.0, 8), (39, 'C', 19.5, 9), (40, 'A', 20.0, 0),
    (41, 'B', 20.5, 1), (42, 'C', 21.0, 2), (43, 'A', 21.5, 3), (44, 'B', 22.0, 4), (45, 'C', 22.5, 5),
    (46, 'A', 23.0, 6), (47, 'B', 23.5, 7), (48, 'C', 24.0, 8), (49, 'A', 24.5, 9), (50, 'B', 25.0, 0);
INSERT INTO perf VALUES
    (51, 'C', 25.5, 1), (52, 'A', 26.0, 2), (53, 'B', 26.5, 3), (54, 'C', 27.0, 4), (55, 'A', 27.5, 5),
    (56, 'B', 28.0, 6), (57, 'C', 28.5, 7), (58, 'A', 29.0, 8), (59, 'B', 29.5, 9), (60, 'C', 30.0, 0),
    (61, 'A', 30.5, 1), (62, 'B', 31.0, 2), (63, 'C', 31.5, 3), (64, 'A', 32.0, 4), (65, 'B', 32.5, 5),
    (66, 'C', 33.0, 6), (67, 'A', 33.5, 7), (68, 'B', 34.0, 8), (69, 'C', 34.5, 9), (70, 'A', 35.0, 0),
    (71, 'B', 35.5, 1), (72, 'C', 36.0, 2), (73, 'A', 36.5, 3), (74, 'B', 37.0, 4), (75, 'C', 37.5, 5),
    (76, 'A', 38.0, 6), (77, 'B', 38.5, 7), (78, 'C', 39.0, 8), (79, 'A', 39.5, 9), (80, 'B', 40.0, 0),
    (81, 'C', 40.5, 1), (82, 'A', 41.0, 2), (83, 'B', 41.5, 3), (84, 'C', 42.0, 4), (85, 'A', 42.5, 5),
    (86, 'B', 43.0, 6), (87, 'C', 43.5, 7), (88, 'A', 44.0, 8), (89, 'B', 44.5, 9), (90, 'C', 45.0, 0),
    (91, 'A', 45.5, 1), (92, 'B', 46.0, 2), (93, 'C', 46.5, 3), (94, 'A', 47.0, 4), (95, 'B', 47.5, 5),
    (96, 'C', 48.0, 6), (97, 'A', 48.5, 7), (98, 'B', 49.0, 8), (99, 'C', 49.5, 9), (100, 'A', 50.0, 0);
INSERT INTO perf VALUES
    (101, 'B', 50.5, 1), (102, 'C', 51.0, 2), (103, 'A', 51.5, 3), (104, 'B', 52.0, 4), (105, 'C', 52.5, 5),
    (106, 'A', 53.0, 6), (107, 'B', 53.5, 7), (108, 'C', 54.0, 8), (109, 'A', 54.5, 9), (110, 'B', 55.0, 0),
    (111, 'C', 55.5, 1), (112, 'A', 56.0, 2), (113, 'B', 56.5, 3), (114, 'C', 57.0, 4), (115, 'A', 57.5, 5),
    (116, 'B', 58.0, 6), (117, 'C', 58.5, 7), (118, 'A', 59.0, 8), (119, 'B', 59.5, 9), (120, 'C', 60.0, 0),
    (121, 'A', 60.5, 1), (122, 'B', 61.0, 2), (123, 'C', 61.5, 3), (124, 'A', 62.0, 4), (125, 'B', 62.5, 5),
    (126, 'C', 63.0, 6), (127, 'A', 63.5, 7), (128, 'B', 64.0, 8), (129, 'C', 64.5, 9), (130, 'A', 65.0, 0),
    (131, 'B', 65.5, 1), (132, 'C', 66.0, 2), (133, 'A', 66.5, 3), (134, 'B', 67.0, 4), (135, 'C', 67.5, 5),
    (136, 'A', 68.0, 6), (137, 'B', 68.5, 7), (138, 'C', 69.0, 8), (139, 'A', 69.5, 9), (140, 'B', 70.0, 0),
    (141, 'C', 70.5, 1), (142, 'A', 71.0, 2), (143, 'B', 71.5, 3), (144, 'C', 72.0, 4), (145, 'A', 72.5, 5),
    (146, 'B', 73.0, 6), (147, 'C', 73.5, 7), (148, 'A', 74.0, 8), (149, 'B', 74.5, 9), (150, 'C', 75.0, 0);
SELECT COUNT(*) FROM perf;
SELECT cat, COUNT(*), AVG(score), SUM(val) FROM perf GROUP BY cat HAVING COUNT(*) >= 50;
SELECT id, score FROM perf WHERE val = 7 ORDER BY score DESC LIMIT 5;
SELECT COUNT(*) FROM perf WHERE score BETWEEN 20.0 AND 40.0;
SELECT cat, MIN(score), MAX(score) FROM perf GROUP BY cat;

exit;
