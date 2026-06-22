# ClickBench Local Testing Guide

This directory is used to perform ClickBench-style functional correctness and basic performance tests on modified database source code in a local environment.

These tests are not part of the official ClickBench submission process, nor are they intended for public performance comparisons with other database systems. Their main goal is to quickly verify whether database query execution is correct and stable, and whether there are obvious regressions or crash issues, when the real ClickBench dataset is too large and inconvenient to run fully in a virtual machine. To achieve this, a smaller synthetic dataset is generated using `generate_series` and `random()`.

## Directory File Description

This directory contains the following files:

| File | Description |
| --- | --- |
| `README.md` | The current documentation file, containing dataset information, test objectives, and test steps. |
| `create.sql` | Creates a ClickBench-compatible `hits` table schema. |
| `random_generate_data.sql` | Randomly generates test data using built-in functions. By default, it generates 10,000,000 rows. |
| `queries.sql` | A collection of ClickBench-style queries, containing 43 queries in total, used to test scenarios such as aggregation, filtering, sorting, grouping, deduplication, and string matching. |

## Test Objectives

This test is mainly used for the following scenarios:

1. Verify whether the database source code can be compiled, started, and used to execute queries normally.
2. Verify whether wide-table operations, large-scale data insertion, and scanning work properly.
3. Verify whether common OLAP query scenarios can be executed normally, including:
    - full table scans;
    - conditional filtering;
    - `GROUP BY` aggregation;
    - `COUNT(DISTINCT ...)`;
    - `ORDER BY ... LIMIT`;
    - string matching, such as `LIKE` and `REGEXP_REPLACE`;
    - date filtering and time truncation, such as `DATE_TRUNC`;
    - multi-column expression calculation.
4. Provide a local regression testing entry point for changes related to the executor, optimizer, expression evaluation, aggregation, sorting, hash tables, parallel queries, and other related components.

## Dataset Description

### Original ClickBench Dataset

The original ClickBench dataset comes from real traffic records in large-scale web analytics scenarios and is released after anonymization. The original dataset preserves relatively realistic data distributions and is suitable for comparing analytical query performance across database systems.

The core table of ClickBench is a single wide table named `hits`, which contains many fields related to web visits, users, devices, traffic sources, search terms, URLs, time, geography, and advertising. The official query set covers typical scenarios in clickstream analytics, web analytics, structured logs, and event data analysis.

### Synthetic Data Used in This Directory

Because the official ClickBench dataset is large, and fully downloading, loading, and running it in a regular virtual machine is costly, this directory uses `random_generate_data.sql` to generate a synthetic dataset.

Default generated scale:

```text
10,000,000 rows
```

Generation method:

```sql
FROM generate_series(1, 10000000)
```

Combined with:

```sql
random()
floor()
CASE WHEN
string concatenation
random time-range generation
```

These are used to generate test values for each field in the `hits` table.

### Characteristics of the Synthetic Data

This synthetic dataset has the following characteristics:

1. Its field structure is consistent with the `hits` table defined in `create.sql`.
2. Its data scale is significantly smaller than the official ClickBench dataset, making it more suitable for virtual machine environments.
3. It intentionally generates some data that can match specific filtering conditions in `queries.sql`, such as:
    - `CounterID = 62`
    - `EventDate` between `2013-07-01` and `2013-07-31`
    - `URL LIKE '%google%'`
    - specific `UserID`
    - specific `URLHash`
    - specific `RefererHash`
4. It is suitable for correctness testing, stability testing, and local debugging.
5. It is not suitable for official performance ranking or comparison with official ClickBench results.

Note that the distribution of the synthetic data is randomly generated and cannot fully simulate the real business distribution, compression characteristics, skewed distribution, or field correlations of the official ClickBench dataset. Therefore, this test is more suitable as a smoke test or regression test after PostgreSQL modifications, rather than as a standard benchmark.

## Test Steps

1. Use `create.sql` to create the test table.
2. Use `random_generate_data.sql` to generate random test data.
3. Execute the queries in `queries.sql` and output the results to files.

## FAQ

### 1. What should I do if inserting 10,000,000 rows is too slow?

You can first reduce the number of rows in `random_generate_data.sql`, for example:

```sql
FROM generate_series(1, 1000000)
```

Use 1 million rows first to verify correctness. After confirming there are no issues, increase the scale to 10 million rows.

### 2. What should I do if disk space is insufficient?

You can use a smaller data scale or clean up old databases:

```bash
dropdb clickbench_test
```

You can also check the disk where the PostgreSQL data directory is located:

```bash
df -h
```

### 3. Does an empty query result always indicate an error?

Not necessarily.

Because randomly generated synthetic data is used here, different fields do not necessarily have real business correlations. Some queries with strong combinations of filtering conditions may return only a few results or even empty results. To reduce this situation, `random_generate_data.sql` already includes embedded data for some key filtering conditions, but it still cannot guarantee that all queries will have the same result distribution as the official dataset.

### 4. Can these results be compared with the official ClickBench leaderboard?

No.

The reasons include:

1. The data is not the original official ClickBench data.
2. The data scale is different.
3. The data distribution is different.
4. The virtual machine hardware environment is different.
5. The test process does not strictly follow the official benchmark rules.

Therefore, this directory is more suitable for local development and debugging, and is not suitable for publishing as official benchmark results.

### 5. How to verify whether the results are correct?

1. First export the data from the MyToyDB test data table.
2. Use `create.sql` to create the data table in a PostgreSQL database.
3. Import the data table exported from MyToyDB into the corresponding PostgreSQL data table.
4. Execute the queries in `queries.sql` in PostgreSQL.
5. Compare whether the query output from PostgreSQL is consistent with the query output from MyToyDB, assuming that the execution results from PostgreSQL are absolutely correct.

**## Test Result Recording**

1. Record the output result of each query in a file.
2. Record the IDs of queries that failed to execute correctly.
