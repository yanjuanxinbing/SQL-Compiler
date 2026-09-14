-- 86_rank_window_function.sql
-- Bug 8 repro: RANK() OVER (PARTITION BY ... ORDER BY <aggregate> DESC)
-- returns 1 for every row when the ORDER BY references an aggregate that was
-- materialised by a downstream AggregateExecutor.
--
-- Baseline: SUM(spent) OVER (PARTITION BY country) (window aggregate)
-- already works.  The PARTITION BY path is correct.  The RANK path is not,
-- because the window's ORDER BY expr (`sum(o.total)`) is a FunctionCallExpr
-- whose name is mapped by the aggregate step's cmap to the materialised
-- aggregate column position, but WindowExecutor::Init / RANK evaluate the
-- expr via ExpressionEvaluator, which has no implementation for SUM (and
-- returns NULL), causing every row's sort key to be NULL — ties across the
-- whole partition, and rank collapses to 1.

CREATE TABLE customer(
    id INT,
    name VARCHAR,
    country VARCHAR
);

CREATE TABLE orders(
    id INT,
    cust_id INT,
    total FLOAT
);

INSERT INTO customer VALUES
    (1, 'Alice', 'USA'),
    (2, 'Bob',   'USA'),
    (3, 'Frank', 'USA'),
    (4, 'Cara',  'UK'),
    (5, 'Dan',   'UK'),
    (6, 'Eve',   'JP'),
    (7, 'Gus',   'JP'),
    (8, 'Hana',  'JP');

INSERT INTO orders VALUES
    (101, 1, 100.0),
    (102, 1, 200.0),
    (103, 1,  85.0),
    (104, 2,  50.0),
    (105, 3,  25.0),
    (106, 4, 400.0),
    (107, 4,  18.0),
    (108, 5, 200.0),
    (109, 6, 500.0),
    (110, 7, 600.0),
    (111, 8,  88.0);

-- Bug repro query (Alice/Bob/Frank in USA should rank 1/2/3).
SELECT c.name, c.country, sum(o.total) AS spent,
       rank() OVER (PARTITION BY c.country ORDER BY sum(o.total) DESC) AS rk
FROM customer c JOIN orders o ON o.cust_id = c.id
GROUP BY c.id, c.name, c.country;

-- Companion diagnostics on the same materialised aggregate.
SELECT c.name, c.country, sum(o.total) AS spent,
       row_number() OVER (PARTITION BY c.country ORDER BY sum(o.total) DESC) AS rn,
       dense_rank() OVER (PARTITION BY c.country ORDER BY sum(o.total) DESC) AS drk
FROM customer c JOIN orders o ON o.cust_id = c.id
GROUP BY c.id, c.name, c.country;

-- Same query without PARTITION BY (single global partition).
SELECT c.name, c.country, sum(o.total) AS spent,
       rank() OVER (ORDER BY sum(o.total) DESC) AS g_rk
FROM customer c JOIN orders o ON o.cust_id = c.id
GROUP BY c.id, c.name, c.country;

-- ASC variant — also exercises the same path.
SELECT c.name, c.country, sum(o.total) AS spent,
       rank() OVER (PARTITION BY c.country ORDER BY sum(o.total) ASC) AS rk_asc
FROM customer c JOIN orders o ON o.cust_id = c.id
GROUP BY c.id, c.name, c.country;

-- Baseline window aggregate (already known to work; must still pass).
SELECT c.name, c.country, sum(o.total) AS spent,
       sum(sum(o.total)) OVER (PARTITION BY c.country) AS country_total
FROM customer c JOIN orders o ON o.cust_id = c.id
GROUP BY c.id, c.name, c.country;

exit;
