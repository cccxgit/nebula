# Clauses Reference

> Detailed clause reference for nGQL. For core syntax, see `ngql-syntax.md`. Official docs: https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/8.clauses-and-options/yield/

## YIELD vs RETURN

- **RETURN** — used in openCypher statements (MATCH, WITH, UNWIND)
- **YIELD** — used in native nGQL statements (GO, LOOKUP, FETCH, pipe queries)

Do **not** mix them. `MATCH ... YIELD` and `GO ... RETURN` are invalid.

```ngql
-- YIELD as a standalone statement (expression evaluation)
YIELD 1 + 1 AS result;
YIELD "hello" AS greeting WHERE 1 == 1;

-- YIELD in native nGQL
GO FROM "p1" OVER follows YIELD dst(edge) AS id, follows.weight AS w;
LOOKUP ON person WHERE person.age > 30 YIELD id(vertex), person.name;
FETCH PROP ON person "p1" YIELD person.name, person.age;
```

## SAMPLE

Used **only** with GO. Uniformly samples edges at each traversal step.

```ngql
-- sample_list length must match STEPS max
GO 3 STEPS FROM "p1" OVER follows YIELD dst(edge) SAMPLE [1, 2, 3];
-- Step 1: sample 1 edge, Step 2: sample 2, Step 3: sample 3

GO 1 TO 3 STEPS FROM "p1" OVER * YIELD properties($$) SAMPLE [1, 2, 4];
```

If fewer edges exist than specified, returns all available.

## INNER JOIN

Joins results of two sub-queries by matching column values.

```ngql
YIELD <columns> FROM <table1> INNER JOIN <table2> ON <join_condition>;
```

- Only `==` comparisons in ON clause
- Table names must differ (use variables to alias)

```ngql
$a = LOOKUP ON person WHERE person.name == "Tim" YIELD id(vertex) AS vid;
$b = GO FROM "p1", "p2" OVER follows YIELD src(edge) AS src, dst(edge) AS dst;
YIELD $a.vid, $b.dst FROM $a INNER JOIN $b ON $a.vid == $b.src;
```

## WHERE

Used in MATCH, LOOKUP, GO (with YIELD), and pipe queries.

Key operators: `==`, `!=`/`<>`, `>`, `>=`, `<`, `<=`, `IN`, `NOT IN`, `STARTS WITH`, `ENDS WITH`, `CONTAINS`, `IS NULL`, `IS NOT NULL`, `IS EMPTY`, `IS NOT EMPTY`, `=~` (regex, openCypher only), `AND`, `OR`, `NOT`, `XOR`.

See `operators.md` for full operator details.

## WITH

Pipes intermediate results within openCypher queries. Acts like a subquery boundary.

```ngql
-- Filter aggregated results
MATCH (v:person)-[:follows]->(v2)
WITH v2, count(*) AS cnt WHERE cnt > 5
RETURN v2.person.name, cnt ORDER BY cnt DESC;

-- Reformat data between MATCH clauses
MATCH (v:person) WHERE v.person.age > 30
WITH v, v.person.name AS name
MATCH (v)-[:follows]->(friend)
RETURN name, friend;
```

## UNWIND

Expands a list into individual rows.

```ngql
UNWIND [1, 2, 3] AS x RETURN x;

-- Batch operations from a list
UNWIND ["p1", "p2", "p3"] AS vid
MATCH (v) WHERE id(v) == vid RETURN v;

-- Flatten collected results
MATCH (v:person)-[:follows]->(v2)
WITH COLLECT(v2) AS friends
UNWIND friends AS f
RETURN DISTINCT f;
```

## ORDER BY / LIMIT / SKIP

```ngql
-- ORDER BY (in RETURN/YIELD)
MATCH (v:person) RETURN v.person.name ORDER BY v.person.age DESC;
MATCH (v:person) RETURN v.person.name ORDER BY v.person.age ASC, v.person.name DESC;

-- LIMIT and SKIP
MATCH (v:person) RETURN v LIMIT 10;
MATCH (v:person) RETURN v SKIP 20 LIMIT 10;  -- pagination
```

## GROUP BY

Implicit in nGQL — non-aggregated columns in RETURN/YIELD automatically act as grouping keys.

```ngql
-- No explicit GROUP BY keyword needed
MATCH (v:person)-[:follows]->(v2)
RETURN v.person.name, COUNT(v2) AS cnt ORDER BY cnt DESC;
```
