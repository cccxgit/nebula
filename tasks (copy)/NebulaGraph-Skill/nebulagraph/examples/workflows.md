# NebulaGraph Examples & Best Practices

> Companion to `../SKILL.md`. For syntax reference see `../references/ngql-syntax.md`; for schema design see `../references/data-modeling.md`.

## 1. Exploration Workflow

Step-by-step approach when encountering an unfamiliar NebulaGraph instance:

```
1. list_spaces()                          → see what's available
2. get_space_schema(space="my_graph")     → inspect tags, edges, indexes
3. find_neighbors(vertex="any_known_id", space="my_graph")  → sample real data
4. execute_query(query="MATCH (v:person) RETURN v LIMIT 5", space="my_graph")  → browse a tag
5. execute_query(query="MATCH ()-[e:follows]->() RETURN e LIMIT 5", space="my_graph")  → browse an edge type
```

Tips:
- Always call `get_space_schema` before writing queries — it reveals tag/edge names, property types, and existing indexes
- Use `find_neighbors` with depth=1 to see what a vertex looks like in context
- Sample multiple tags to understand the data distribution

## 2. Query Composition

Build queries incrementally — start simple, add complexity:

```ngql
-- Step 1: Basic — does the vertex exist?
MATCH (v) WHERE id(v) == "user_001" RETURN v;

-- Step 2: Add edge traversal
MATCH (v)-[e:follows]->(v2) WHERE id(v) == "user_001" RETURN v2;

-- Step 3: Filter on properties
MATCH (v)-[e:follows]->(v2:person) WHERE id(v) == "user_001" AND v2.person.age > 25 RETURN v2;

-- Step 4: Multi-hop
MATCH (v)-[e:follows*1..2]->(v2:person) WHERE id(v) == "user_001" AND ALL(x IN e WHERE x.weight > 0.5) RETURN DISTINCT v2;

-- Step 5: Aggregate
MATCH (v)-[e:follows*1..2]->(v2:person) WHERE id(v) == "user_001"
WITH v2, COUNT(*) AS paths
RETURN v2.person.name, paths ORDER BY paths DESC LIMIT 10;
```

When a query fails, simplify back to the last working step and debug from there.

## 3. Schema Evolution

Adding to an existing space (no downtime needed):

```ngql
-- Add a new tag
CREATE TAG IF NOT EXISTS company (name STRING NOT NULL, founded DATE);
-- Wait ~20s for heartbeat sync

-- Add property to existing tag
ALTER TAG person ADD (email STRING);

-- Add a new edge type
CREATE EDGE IF NOT EXISTS works_at (since DATE, role STRING);

-- Create indexes for new schema elements
CREATE TAG INDEX idx_company ON company();
CREATE TAG INDEX idx_company_name ON company(name(64));
CREATE EDGE INDEX idx_works_at ON works_at();
REBUILD TAG INDEX idx_company;
REBUILD TAG INDEX idx_company_name;
REBUILD EDGE INDEX idx_works_at;
-- Check: SHOW TAG INDEX STATUS; (wait for FINISHED)
```

Migration approach:
1. CREATE new tags/edges/indexes
2. Wait for heartbeat (~20s) + index rebuild
3. INSERT new data
4. Verify with sample queries
5. DROP old schema elements only after confirming no queries depend on them

## 4. Graph Analysis Patterns

```ngql
-- Degree centrality (most connected vertices)
MATCH (v:person)-[e:follows]-() RETURN id(v), v.person.name, COUNT(e) AS degree ORDER BY degree DESC LIMIT 20;

-- Find bridges (vertices connecting otherwise separate groups)
MATCH (a:person)-[:follows]->(bridge:person)-[:follows]->(b:person)
WHERE NOT (a)-[:follows]->(b) AND id(a) != id(b)
RETURN bridge.person.name, COUNT(*) AS bridge_count ORDER BY bridge_count DESC LIMIT 10;

-- Influence paths (who can reach whom within N hops)
MATCH p = (a)-[e:follows*1..3]->(b) WHERE id(a) == "influencer_001"
RETURN id(b), length(p) AS distance, COUNT(p) AS num_paths ORDER BY distance, num_paths DESC;

-- Mutual connections between two people
MATCH (a)-[:follows]->(m)<-[:follows]-(b) WHERE id(a) == "user_001" AND id(b) == "user_002"
RETURN m.person.name;

-- Subgraph extraction for community analysis
GET SUBGRAPH 3 STEPS FROM "user_001" BOTH follows YIELD VERTICES AS v, EDGES AS e;
```

## 5. Bulk Data Operations

```ngql
-- Batch insert (100-200 rows per statement for best throughput)
INSERT VERTEX person (name, age) VALUES
  "p1":("Alice", 30), "p2":("Bob", 25), "p3":("Carol", 28),
  "p4":("Dave", 35), "p5":("Eve", 22);

INSERT EDGE follows (since, weight) VALUES
  "p1"->"p2":(datetime("2023-01-01T00:00:00"), 0.9),
  "p1"->"p3":(datetime("2023-02-01T00:00:00"), 0.7),
  "p2"->"p4":(datetime("2023-03-01T00:00:00"), 0.8);

-- Bulk delete by condition
LOOKUP ON person WHERE person.age < 18 YIELD id(vertex) AS vid | DELETE VERTEX $-.vid WITH EDGE;

-- Bulk delete by edge type from a vertex
GO FROM "p1" OVER follows YIELD id($^) AS src, dst(edge) AS dst, rank(edge) AS r
| DELETE EDGE follows $-.src -> $-.dst @ $-.r;
```

For large imports (millions of rows): use NebulaGraph Importer or Spark Connector instead of nGQL INSERT. Create indexes after import, then REBUILD.

## 6. Error Diagnosis

Common errors and fixes:

| Error | Cause | Fix |
|-------|-------|-----|
| `SemanticError: ... not found` | Tag/edge/property doesn't exist | Check spelling; run `get_space_schema` to verify names |
| `SemanticError: To get the property of a vertex in MATCH, use the format v.tag.prop` | Used `v.prop` instead of `v.tag.prop` | Always include tag: `v.person.name` |
| `ExecutionError: Storage Error: The VID must be a 64-bit integer or a string` | VID type mismatch | Check space VID type; string VIDs need `"quotes"` |
| `SemanticError: Index not found for ...` | LOOKUP/MATCH by tag without index | CREATE + REBUILD index first |
| `SemanticError: ... is not a valid expression` | Using `=` instead of `==` in WHERE | Use `==` for equality comparison |
| `ExecutionError: ... edge not found` | Schema change not yet propagated | Wait ~20s after CREATE/ALTER for heartbeat sync |
| `SyntaxError: syntax error near ...` | nGQL syntax issue | Check quotes (double for strings), semicolons, keyword spelling |
| `ExecutionError: The leader has changed` | Cluster instability / partition rebalancing | Retry after a few seconds; check cluster health |

Debugging approach:
1. Simplify the query to isolate the failing part
2. Use `PROFILE` prefix to see execution plan and identify bottlenecks
3. Verify schema with `get_space_schema` — property names and types
4. Check index status: `SHOW TAG INDEX STATUS` / `SHOW EDGE INDEX STATUS`

## 7. MCP Tool Combination Patterns

**Pattern A: Explore → Query → Refine**
```
get_space_schema(space="social")           → learn the schema
find_neighbors(vertex="user_1", space="social")  → see data shape
execute_query(query="MATCH ...", space="social")  → targeted query
```

**Pattern B: Schema Design → Create → Verify**
```
# Read ../references/data-modeling.md for design guidance, then:
execute_query(query="CREATE TAG ...", space="my_space")
# Wait ~20s
execute_query(query="CREATE TAG INDEX ...", space="my_space")
execute_query(query="REBUILD TAG INDEX ...", space="my_space")
get_space_schema(space="my_space")         → verify schema created correctly
```

**Pattern C: Path Finding → Detail Enrichment**
```
find_path(src="user_1", dst="user_2", space="social")  → get path topology
# Then fetch details for vertices in the path:
execute_query(query="FETCH PROP ON person 'v1', 'v2', 'v3' YIELD properties(vertex)", space="social")
```

**Pattern D: Broad Exploration → Focused Analysis**
```
find_neighbors(vertex="hub_node", space="social", depth=2)  → broad view
execute_query(query="MATCH (v)-[e:follows]-() WHERE id(v) == 'hub_node' RETURN COUNT(e)", space="social")  → degree count
execute_query(query="PROFILE MATCH ...", space="social")     → performance check
```

When to use tools vs raw nGQL:
- `find_path` / `find_neighbors`: Quick exploration, no nGQL knowledge needed. Limited customization.
- `execute_query` with MATCH/GO: Full control over filters, aggregation, output format. Use when tools don't offer enough flexibility.

## 8. Common Query Recipes

Quick-reference patterns for frequent tasks:

```ngql
-- Two-degree connections (friends of friends)
MATCH (v)-[:follows*2]->(fof) WHERE id(v) == "p1" AND id(fof) != id(v) RETURN DISTINCT fof;

-- Count by tag
LOOKUP ON person YIELD id(vertex) | YIELD COUNT(*) AS total;

-- Top-N by property
MATCH (v:person) RETURN v.person.name, v.person.age ORDER BY v.person.age DESC LIMIT 10;

-- Conditional labeling (CASE WHEN)
MATCH (v:person) RETURN v.person.name, CASE WHEN v.person.age >= 30 THEN "senior" ELSE "junior" END AS cat;

-- Aggregation (implicit GROUP BY)
MATCH (v:person)-[:follows]->(v2) RETURN v.person.name, COUNT(v2) AS cnt ORDER BY cnt DESC;
```
