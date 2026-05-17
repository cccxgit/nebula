# nGQL Reference

> Core nGQL syntax reference. For workflow/MCP tool guidance, see `../SKILL.md`. For schema design, see `data-modeling.md`. For examples, see `../examples/workflows.md`. For clauses (YIELD, WITH, UNWIND, GROUP BY), see `clauses.md`. For functions, see `functions.md`.

## Data Types

`BOOL`, `INT8`/`INT16`/`INT32`/`INT64`, `FLOAT`, `DOUBLE`, `STRING`, `FIXED_STRING(N)`, `DATE`, `TIME`, `DATETIME`, `TIMESTAMP`, `GEOGRAPHY`, `DURATION`. All nullable unless `NOT NULL`. `LIST`/`SET`/`MAP` in expressions only, not as property types.

## Space Management

```ngql
CREATE SPACE [IF NOT EXISTS] s (vid_type=FIXED_STRING(64), partition_num=20, replica_factor=1);
CREATE SPACE new_s AS old_s;  -- clone schema only
USE s; SHOW SPACES; DESCRIBE SPACE s; DROP SPACE [IF EXISTS] s;
CLEAR SPACE s;  -- delete data, keep schema
```

**Warning**: Wait ~20s after `CREATE SPACE` before creating schema (heartbeat sync).

## Schema DDL

```ngql
CREATE TAG [IF NOT EXISTS] person (name STRING NOT NULL, age INT32 DEFAULT 0, bio STRING)
  TTL_DURATION = 0, TTL_COL = "", COMMENT = "Person tag";
ALTER TAG person ADD (phone STRING); ALTER TAG person CHANGE (age INT64); ALTER TAG person DROP (bio);
SHOW TAGS; DESCRIBE TAG person; DROP TAG [IF EXISTS] person;

CREATE EDGE [IF NOT EXISTS] follows (since DATETIME NOT NULL, weight DOUBLE DEFAULT 1.0);
ALTER EDGE follows ADD (note STRING); ALTER EDGE follows DROP (note);
SHOW EDGES; DESCRIBE EDGE follows; DROP EDGE [IF EXISTS] follows;
```

TTL: set `TTL_DURATION` (seconds) + `TTL_COL` (`DATE`/`DATETIME`/`INT64` column) to auto-expire.

## Data DML

```ngql
-- Insert (batch is faster)
INSERT VERTEX person (name, age) VALUES "p1":("Tim", 34), "p2":("Tony", 36);
INSERT VERTEX IF NOT EXISTS person (name, age) VALUES "p1":("Tim", 34);
INSERT EDGE follows (since, weight) VALUES "p1"->"p2":(datetime("2020-01-01T00:00:00"), 0.9);
INSERT EDGE follows (since) VALUES "p1"->"p2"@1:(datetime("2020-01-01T00:00:00"));  -- with rank

-- Update / Upsert
UPDATE VERTEX ON person "p1" SET age = 35;
UPDATE EDGE ON follows "p1"->"p2"@0 SET weight = 0.95;
UPSERT VERTEX ON person "p1" SET name = "Tim", age = age + 1 WHEN age >= 34 YIELD name, age;

-- Delete (always use WITH EDGE for vertices)
DELETE VERTEX "p1" WITH EDGE;
DELETE EDGE follows "p1"->"p2"@0;
```

## MATCH Patterns

```ngql
-- By tag (requires tag index)
MATCH (v:person) RETURN v LIMIT 10;
-- By VID (no index needed)
MATCH (v) WHERE id(v) == "p1" RETURN v;
-- By property (requires property index): note v.tag.prop syntax
MATCH (v:person) WHERE v.person.name == "Tim" RETURN v;

-- Edge patterns
MATCH (a:person)-[e:follows]->(b:person) WHERE id(a) == "p1" RETURN b, e;
MATCH (v)-[e:follows|:serves]->(v2) WHERE id(v) == "p1" RETURN type(e), v2;  -- multi-type
MATCH (v)-[e]-(v2) WHERE id(v) == "p1" RETURN v2;  -- undirected, any type

-- Variable-length (e is a LIST — filter with ALL())
MATCH (a)-[e:follows*2]->(b) WHERE id(a) == "p1" RETURN b;  -- exactly 2 hops
MATCH (a)-[e:follows*1..3]->(b) WHERE id(a) == "p1" AND ALL(x IN e WHERE x.weight > 0.5) RETURN b;

-- Shortest path
MATCH p = shortestPath((a)-[e:follows*..5]-(b)) WHERE id(a) == "p1" AND id(b) == "p2" RETURN p;
MATCH p = allShortestPaths((a)-[e*..5]-(b)) WHERE id(a) == "p1" AND id(b) == "p2" RETURN p;

-- WITH clause (pipe intermediate results)
MATCH (v:person)-[:follows]->(v2) WHERE v.person.age > 30
WITH v2, count(*) AS cnt WHERE cnt > 5
RETURN v2.person.name, cnt ORDER BY cnt DESC LIMIT 10;

-- OPTIONAL MATCH (like LEFT JOIN)
MATCH (v:person) WHERE id(v) == "p1" OPTIONAL MATCH (v)-[e:follows]->(v2) RETURN v, v2;
```

**WHERE operators**: `==`, `!=`/`<>`, `>`/`>=`/`<`/`<=`, `IN`, `NOT IN`, `STARTS WITH`, `ENDS WITH`, `CONTAINS`, `IS NULL`/`IS NOT NULL`, `=~` (regex), `AND`/`OR`/`NOT`/`XOR`.

**RETURN**: supports `AS` alias, `DISTINCT`, `ORDER BY ... ASC/DESC`, `SKIP N`, `LIMIT N`.

## GO Traversal

```ngql
GO FROM "p1" OVER follows YIELD dst(edge) AS dst;
GO 2 STEPS FROM "p1" OVER follows YIELD dst(edge);  -- exactly 2 hops
GO 1 TO 3 STEPS FROM "p1" OVER follows YIELD dst(edge);  -- range
GO FROM "p1" OVER follows REVERSELY YIELD src(edge);
GO FROM "p1" OVER follows BIDIRECT YIELD src(edge), dst(edge);
GO FROM "p1" OVER follows, serves YIELD dst(edge), properties(edge);

-- Pipe chaining
GO FROM "p1" OVER follows YIELD dst(edge) AS id | GO FROM $-.id OVER serves YIELD dst(edge);
```

Edge functions: `src(edge)`, `dst(edge)`, `type(edge)`, `rank(edge)`, `properties(edge)`.

## FIND PATH

```ngql
FIND SHORTEST PATH FROM "p1" TO "p2" OVER follows YIELD path AS p;
FIND ALL PATH FROM "p1" TO "p2" OVER follows UPTO 5 STEPS YIELD path AS p;
FIND NOLOOP PATH FROM "p1" TO "p2" OVER follows UPTO 5 STEPS YIELD path AS p;
FIND SHORTEST PATH WITH PROP FROM "p1" TO "p2" OVER follows YIELD path AS p;
-- Multiple sources/destinations supported
```

## LOOKUP

Requires a **pre-built index** on the tag/edge property.

```ngql
LOOKUP ON person YIELD id(vertex), person.name;
LOOKUP ON person WHERE person.age > 30 YIELD id(vertex), person.name, person.age;
LOOKUP ON follows WHERE follows.weight > 0.8 YIELD src(edge), dst(edge), follows.weight;
```

## FETCH

No index required. Retrieves properties for known VIDs/edges.

```ngql
FETCH PROP ON person "p1" YIELD properties(vertex);
FETCH PROP ON person "p1", "p2" YIELD person.name, person.age;
FETCH PROP ON follows "p1"->"p2"@0 YIELD properties(edge);
FETCH PROP ON * "p1" YIELD vertex AS v;  -- all tags
```

## Pipe & Reference Symbols

`|` pipes results. `$-.col` = piped column, `$^.tag.prop` = source vertex prop, `$$.tag.prop` = dest vertex prop.

```ngql
GO FROM "p1" OVER follows YIELD follows.weight, $$.person.name AS friend;
```

## Subgraph Queries

```ngql
GET SUBGRAPH 2 STEPS FROM "p1" YIELD VERTICES AS v, EDGES AS e;
GET SUBGRAPH 2 STEPS FROM "p1" OUT follows YIELD VERTICES AS v, EDGES AS e;
GET SUBGRAPH 2 STEPS FROM "p1" BOTH follows, serves YIELD VERTICES AS v, EDGES AS e;
```

## Built-in Functions

**Math**: `abs`, `floor`, `ceil`, `round`, `sqrt`, `pow`, `log`, `rand`, `pi`, `e`
**String**: `lower`, `upper`, `trim`, `left`, `right`, `length`, `substr`, `replace`, `reverse`, `split`, `concat`, `concat_ws`, `toString`
**Date/Time**: `now`, `date`, `time`, `datetime`, `timestamp` + accessor `.year/.month/.day/.hour/.minute/.second`
**Aggregation**: `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`, `COLLECT`, `STD` (use `COLLECT` + `toSet()` for distinct collection)
**List**: `size`, `range`, `head`, `last`, `tail`, `reduce`, list comprehension `[x IN list WHERE cond | expr]`
**Type**: `toInteger`, `toFloat`, `toBoolean`, `toString`, `toSet`, `hash`
**Graph**: `id(vertex)`, `properties(v/e)`, `tags(vertex)`, `type(edge)`, `src(edge)`, `dst(edge)`, `rank(edge)`, `vertices(path)`, `edges(path)`, `length(path)`, `nodes(path)`, `relationships(path)`

For complete function signatures, see `functions.md`.

## Index Management

```ngql
CREATE TAG INDEX idx_person ON person();                    -- tag-level (for MATCH by tag)
CREATE TAG INDEX idx_name ON person(name(20));              -- STRING needs length; FIXED_STRING doesn't
CREATE TAG INDEX idx_name_age ON person(name(20), age);     -- composite (leftmost prefix rule)
CREATE EDGE INDEX idx_weight ON follows(weight);

REBUILD TAG INDEX idx_person, idx_name;  -- required before use
REBUILD EDGE INDEX idx_weight;
SHOW TAG INDEX STATUS; SHOW EDGE INDEX STATUS;  -- wait for FINISHED
SHOW TAG INDEXES; DESCRIBE TAG INDEX idx_name; DROP TAG INDEX [IF EXISTS] idx_name;
```

**Leftmost prefix**: index on `(name, age)` works for `name` or `name+age`, NOT `age` alone.

## EXPLAIN / PROFILE

```ngql
EXPLAIN MATCH (v:person)-[:follows]->(v2) WHERE id(v) == "p1" RETURN v2;  -- show query plan without executing
PROFILE MATCH (v:person)-[:follows]->(v2) WHERE id(v) == "p1" RETURN v2;  -- execute and show plan + metrics
PROFILE format="dot" GO FROM "p1" OVER follows YIELD dst(edge);           -- DOT format for visualization
```

`EXPLAIN` shows the execution plan without running the query. `PROFILE` runs the query and returns plan + row counts + execution time per operator. Use `PROFILE` to identify slow operators and optimize.
