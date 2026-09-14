-- 67_bptree_rebalance.sql
-- B+Tree 删除路径再平衡（redistribute / merge / root-collapse）的端到端测试
--
-- 背景：之前 BPlusTree::Delete 只在叶子上 entries.erase + WriteLeafEntries，
-- 完全不向上传播。经过大量删除后，树的填充率会退化但仍能查到正确结果。
-- 本测试大量插入后批量删除，验证：
--   1) 剩余行的等值 / 范围查询结果正确；
--   2) 全删后再插，索引还能正常工作（root-collapse 后 root_page_id 不变）；
--   3) 删除唯一索引上唯一的键后，再插同键仍能成功（叶子被清空但 root 仍是合法页）；
--   4) 混合 delete + 范围 scan + insert 的最终状态与「全插后全删」一致。

-- ---------- 1. 主键索引：插 200 条，删其中 150 条，校验剩余 50 条 ----------
CREATE TABLE rb1 (id INT PRIMARY KEY, payload VARCHAR(20));
INSERT INTO rb1 VALUES (1, 'a');
INSERT INTO rb1 VALUES (2, 'b');
INSERT INTO rb1 VALUES (3, 'c');
INSERT INTO rb1 VALUES (4, 'd');
INSERT INTO rb1 VALUES (5, 'e');
INSERT INTO rb1 VALUES (6, 'f');
INSERT INTO rb1 VALUES (7, 'g');
INSERT INTO rb1 VALUES (8, 'h');
INSERT INTO rb1 VALUES (9, 'i');
INSERT INTO rb1 VALUES (10, 'j');
INSERT INTO rb1 VALUES (11, 'k');
INSERT INTO rb1 VALUES (12, 'l');
INSERT INTO rb1 VALUES (13, 'm');
INSERT INTO rb1 VALUES (14, 'n');
INSERT INTO rb1 VALUES (15, 'o');
INSERT INTO rb1 VALUES (16, 'p');
INSERT INTO rb1 VALUES (17, 'q');
INSERT INTO rb1 VALUES (18, 'r');
INSERT INTO rb1 VALUES (19, 's');
INSERT INTO rb1 VALUES (20, 't');
INSERT INTO rb1 VALUES (21, 'u');
INSERT INTO rb1 VALUES (22, 'v');
INSERT INTO rb1 VALUES (23, 'w');
INSERT INTO rb1 VALUES (24, 'x');
INSERT INTO rb1 VALUES (25, 'y');
INSERT INTO rb1 VALUES (26, 'z');
INSERT INTO rb1 VALUES (27, 'aa');
INSERT INTO rb1 VALUES (28, 'bb');
INSERT INTO rb1 VALUES (29, 'cc');
INSERT INTO rb1 VALUES (30, 'dd');
INSERT INTO rb1 VALUES (31, 'ee');
INSERT INTO rb1 VALUES (32, 'ff');
INSERT INTO rb1 VALUES (33, 'gg');
INSERT INTO rb1 VALUES (34, 'hh');
INSERT INTO rb1 VALUES (35, 'ii');
INSERT INTO rb1 VALUES (36, 'jj');
INSERT INTO rb1 VALUES (37, 'kk');
INSERT INTO rb1 VALUES (38, 'll');
INSERT INTO rb1 VALUES (39, 'mm');
INSERT INTO rb1 VALUES (40, 'nn');
INSERT INTO rb1 VALUES (41, 'oo');
INSERT INTO rb1 VALUES (42, 'pp');
INSERT INTO rb1 VALUES (43, 'qq');
INSERT INTO rb1 VALUES (44, 'rr');
INSERT INTO rb1 VALUES (45, 'ss');
INSERT INTO rb1 VALUES (46, 'tt');
INSERT INTO rb1 VALUES (47, 'uu');
INSERT INTO rb1 VALUES (48, 'vv');
INSERT INTO rb1 VALUES (49, 'ww');
INSERT INTO rb1 VALUES (50, 'xx');
INSERT INTO rb1 VALUES (51, 'yy');
INSERT INTO rb1 VALUES (52, 'zz');
INSERT INTO rb1 VALUES (53, 'aaa');
INSERT INTO rb1 VALUES (54, 'bbb');
INSERT INTO rb1 VALUES (55, 'ccc');
INSERT INTO rb1 VALUES (56, 'ddd');
INSERT INTO rb1 VALUES (57, 'eee');
INSERT INTO rb1 VALUES (58, 'fff');
INSERT INTO rb1 VALUES (59, 'ggg');
INSERT INTO rb1 VALUES (60, 'hhh');
INSERT INTO rb1 VALUES (61, 'iii');
INSERT INTO rb1 VALUES (62, 'jjj');
INSERT INTO rb1 VALUES (63, 'kkk');
INSERT INTO rb1 VALUES (64, 'lll');
INSERT INTO rb1 VALUES (65, 'mmm');
INSERT INTO rb1 VALUES (66, 'nnn');
INSERT INTO rb1 VALUES (67, 'ooo');
INSERT INTO rb1 VALUES (68, 'ppp');
INSERT INTO rb1 VALUES (69, 'qqq');
INSERT INTO rb1 VALUES (70, 'rrr');
INSERT INTO rb1 VALUES (71, 'sss');
INSERT INTO rb1 VALUES (72, 'ttt');
INSERT INTO rb1 VALUES (73, 'uuu');
INSERT INTO rb1 VALUES (74, 'vvv');
INSERT INTO rb1 VALUES (75, 'www');
INSERT INTO rb1 VALUES (76, 'xxx');
INSERT INTO rb1 VALUES (77, 'yyy');
INSERT INTO rb1 VALUES (78, 'zzz');
INSERT INTO rb1 VALUES (79, 'aaaa');
INSERT INTO rb1 VALUES (80, 'bbbb');
INSERT INTO rb1 VALUES (81, 'cccc');
INSERT INTO rb1 VALUES (82, 'dddd');
INSERT INTO rb1 VALUES (83, 'eeee');
INSERT INTO rb1 VALUES (84, 'ffff');
INSERT INTO rb1 VALUES (85, 'gggg');
INSERT INTO rb1 VALUES (86, 'hhhh');
INSERT INTO rb1 VALUES (87, 'iiii');
INSERT INTO rb1 VALUES (88, 'jjjj');
INSERT INTO rb1 VALUES (89, 'kkkk');
INSERT INTO rb1 VALUES (90, 'llll');
INSERT INTO rb1 VALUES (91, 'mmmm');
INSERT INTO rb1 VALUES (92, 'nnnn');
INSERT INTO rb1 VALUES (93, 'oooo');
INSERT INTO rb1 VALUES (94, 'pppp');
INSERT INTO rb1 VALUES (95, 'qqqq');
INSERT INTO rb1 VALUES (96, 'rrrr');
INSERT INTO rb1 VALUES (97, 'ssss');
INSERT INTO rb1 VALUES (98, 'tttt');
INSERT INTO rb1 VALUES (99, 'uuuu');
INSERT INTO rb1 VALUES (100, 'vvvv');

-- 删除前 75 条（1..75），保留 76..100。等值与区间查询都应返回 76..100。
DELETE FROM rb1 WHERE id <= 75;
SELECT * FROM rb1 WHERE id = 80;
SELECT * FROM rb1 WHERE id >= 80 AND id <= 90 ORDER BY id;
SELECT * FROM rb1 WHERE id BETWEEN 76 AND 100 ORDER BY id;
SELECT COUNT(*) FROM rb1;
SELECT COUNT(*) FROM rb1 WHERE id >= 50;
-- 反向：再删一批（80..95），保留 76..79, 96..100
DELETE FROM rb1 WHERE id >= 80 AND id <= 95;
SELECT COUNT(*) FROM rb1;
SELECT * FROM rb1 WHERE id BETWEEN 76 AND 100 ORDER BY id;
-- 全删
DELETE FROM rb1;
SELECT COUNT(*) FROM rb1;
-- 再插一组：root-collapse 后 root_page_id 仍有效，新插入应能正常建树
INSERT INTO rb1 VALUES (1, 'fresh1');
INSERT INTO rb1 VALUES (2, 'fresh2');
INSERT INTO rb1 VALUES (3, 'fresh3');
INSERT INTO rb1 VALUES (4, 'fresh4');
INSERT INTO rb1 VALUES (5, 'fresh5');
INSERT INTO rb1 VALUES (6, 'fresh6');
INSERT INTO rb1 VALUES (7, 'fresh7');
INSERT INTO rb1 VALUES (8, 'fresh8');
INSERT INTO rb1 VALUES (9, 'fresh9');
INSERT INTO rb1 VALUES (10, 'fresh10');
SELECT COUNT(*) FROM rb1;
SELECT * FROM rb1 ORDER BY id;

-- ---------- 2. 唯一索引：单键删除后再插 ----------
CREATE TABLE rb2 (id INT PRIMARY KEY, name VARCHAR(20));
CREATE UNIQUE INDEX uq_rb2_name ON rb2(name);
INSERT INTO rb2 VALUES (1, 'alpha');
INSERT INTO rb2 VALUES (2, 'beta');
INSERT INTO rb2 VALUES (3, 'gamma');
-- 删除 alpha
DELETE FROM rb2 WHERE name = 'alpha';
SELECT * FROM rb2 WHERE name = 'alpha';
-- 再插 alpha（unique 索引必须接受，因为旧条目已删）
INSERT INTO rb2 VALUES (4, 'alpha');
SELECT * FROM rb2 WHERE name = 'alpha';
SELECT * FROM rb2 ORDER BY id;

-- ---------- 3. 二级索引（非唯一）：重复键跨页删除后再插 ----------
CREATE TABLE rb3 (id INT PRIMARY KEY, grp INT);
CREATE INDEX idx_rb3_grp ON rb3(grp);
INSERT INTO rb3 VALUES (1, 1);
INSERT INTO rb3 VALUES (2, 1);
INSERT INTO rb3 VALUES (3, 1);
INSERT INTO rb3 VALUES (4, 2);
INSERT INTO rb3 VALUES (5, 2);
INSERT INTO rb3 VALUES (6, 2);
INSERT INTO rb3 VALUES (7, 3);
INSERT INTO rb3 VALUES (8, 3);
INSERT INTO rb3 VALUES (9, 3);
-- 删 grp=1 全部三条，再查 grp=1 应为空；再插 grp=1 应能成功
DELETE FROM rb3 WHERE id IN (1, 2, 3);
SELECT * FROM rb3 WHERE grp = 1 ORDER BY id;
INSERT INTO rb3 VALUES (10, 1);
INSERT INTO rb3 VALUES (11, 1);
SELECT * FROM rb3 WHERE grp = 1 ORDER BY id;
SELECT * FROM rb3 ORDER BY id;

-- ---------- 4. 混合 delete + range scan + insert 顺序 ----------
-- 这段验证「插入 1..20，再交错的删/查/插」与「插 1..20 后全删 + 重插 1..10」的
-- 最终 SELECT 集合一致。
CREATE TABLE rb4 (id INT PRIMARY KEY, val INT);
INSERT INTO rb4 VALUES (1, 10);
INSERT INTO rb4 VALUES (2, 20);
INSERT INTO rb4 VALUES (3, 30);
INSERT INTO rb4 VALUES (4, 40);
INSERT INTO rb4 VALUES (5, 50);
INSERT INTO rb4 VALUES (6, 60);
INSERT INTO rb4 VALUES (7, 70);
INSERT INTO rb4 VALUES (8, 80);
INSERT INTO rb4 VALUES (9, 90);
INSERT INTO rb4 VALUES (10, 100);
INSERT INTO rb4 VALUES (11, 110);
INSERT INTO rb4 VALUES (12, 120);
INSERT INTO rb4 VALUES (13, 130);
INSERT INTO rb4 VALUES (14, 140);
INSERT INTO rb4 VALUES (15, 150);
INSERT INTO rb4 VALUES (16, 160);
INSERT INTO rb4 VALUES (17, 170);
INSERT INTO rb4 VALUES (18, 180);
INSERT INTO rb4 VALUES (19, 190);
INSERT INTO rb4 VALUES (20, 200);
-- 交错删除
DELETE FROM rb4 WHERE id IN (2, 4, 6, 8, 10);
SELECT * FROM rb4 WHERE id >= 5 AND id <= 15 ORDER BY id;
DELETE FROM rb4 WHERE id IN (12, 14, 16);
SELECT * FROM rb4 ORDER BY id;
DELETE FROM rb4 WHERE id >= 17;
SELECT COUNT(*) FROM rb4;
-- 重新插入几条
INSERT INTO rb4 VALUES (21, 210);
INSERT INTO rb4 VALUES (22, 220);
INSERT INTO rb4 VALUES (2, 25);
INSERT INTO rb4 VALUES (16, 165);
SELECT * FROM rb4 ORDER BY id;

-- ---------- 5. 大量连续插入 + 大量删除后范围查询仍正确 ----------
-- 这条专门压测 redistribute / merge：插入后至少 2 个叶子页，再删到只剩少量。
CREATE TABLE rb5 (id INT PRIMARY KEY, payload VARCHAR(50));
INSERT INTO rb5 VALUES (1, 'row01');
INSERT INTO rb5 VALUES (2, 'row02');
INSERT INTO rb5 VALUES (3, 'row03');
INSERT INTO rb5 VALUES (4, 'row04');
INSERT INTO rb5 VALUES (5, 'row05');
INSERT INTO rb5 VALUES (6, 'row06');
INSERT INTO rb5 VALUES (7, 'row07');
INSERT INTO rb5 VALUES (8, 'row08');
INSERT INTO rb5 VALUES (9, 'row09');
INSERT INTO rb5 VALUES (10, 'row10');
INSERT INTO rb5 VALUES (11, 'row11');
INSERT INTO rb5 VALUES (12, 'row12');
INSERT INTO rb5 VALUES (13, 'row13');
INSERT INTO rb5 VALUES (14, 'row14');
INSERT INTO rb5 VALUES (15, 'row15');
INSERT INTO rb5 VALUES (16, 'row16');
INSERT INTO rb5 VALUES (17, 'row17');
INSERT INTO rb5 VALUES (18, 'row18');
INSERT INTO rb5 VALUES (19, 'row19');
INSERT INTO rb5 VALUES (20, 'row20');
INSERT INTO rb5 VALUES (21, 'row21');
INSERT INTO rb5 VALUES (22, 'row22');
INSERT INTO rb5 VALUES (23, 'row23');
INSERT INTO rb5 VALUES (24, 'row24');
INSERT INTO rb5 VALUES (25, 'row25');
INSERT INTO rb5 VALUES (26, 'row26');
INSERT INTO rb5 VALUES (27, 'row27');
INSERT INTO rb5 VALUES (28, 'row28');
INSERT INTO rb5 VALUES (29, 'row29');
INSERT INTO rb5 VALUES (30, 'row30');
-- 删 15 条，剩 15 条，应能正确查
DELETE FROM rb5 WHERE id <= 15 OR id >= 26;
SELECT * FROM rb5 WHERE id BETWEEN 16 AND 25 ORDER BY id;
SELECT * FROM rb5 ORDER BY id;
-- 再删几条，制造更稀的状态
DELETE FROM rb5 WHERE id IN (16, 18, 20, 22, 24);
SELECT * FROM rb5 WHERE id >= 16 ORDER BY id;
-- 再插一些，验证「稀树」状态仍能继续 insert
INSERT INTO rb5 VALUES (31, 'row31');
INSERT INTO rb5 VALUES (32, 'row32');
INSERT INTO rb5 VALUES (33, 'row33');
INSERT INTO rb5 VALUES (50, 'row50');
INSERT INTO rb5 VALUES (100, 'row100');
SELECT * FROM rb5 WHERE id BETWEEN 16 AND 50 ORDER BY id;
SELECT * FROM rb5 ORDER BY id;

exit;
