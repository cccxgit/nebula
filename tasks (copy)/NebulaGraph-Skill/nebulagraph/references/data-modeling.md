# NebulaGraph Data Modeling Guide

> Companion to NebulaGraph `../SKILL.md`. For nGQL syntax, see `ngql-syntax.md`. For examples, see `../examples/workflows.md`.

## Concepts

**Space**: isolation unit (like DB) with own schema/VID type. **Tag**: property set on vertex; vertices can have multiple tags. **Edge Type**: directed, identified by `(src, type, rank, dst)`. **Property**: typed key-value, strong schema. **VID**: `INT64` or `FIXED_STRING(N)`, immutable after space creation.

## Multi-Space vs Single-Space

**Single space** (default): All entity types in one space with different tags. Simpler queries — edges can connect any vertices within the space. Best for most use cases.

**Multiple spaces** when:
- Completely unrelated domains (e.g., social graph + product catalog) with no cross-domain queries
- Different VID types needed (INT64 for one domain, FIXED_STRING for another)
- Isolation for access control or independent scaling
- Different partition/replica configs needed

**Tradeoff**: Cross-space queries are impossible — no JOINs between spaces. If entities might relate, keep them in one space with distinct tag prefixes.

## VID Design

| Factor | `INT64` | `FIXED_STRING(N)` |
|--------|---------|-------------------|
| Storage | 8 bytes | N bytes |
| Performance | Fastest | Slightly slower |
| Readability | Opaque | Human-readable |
| External keys | Need mapping | Use natural keys |

Use `FIXED_STRING` with natural keys (user IDs, SKUs). Use `INT64` for max performance. Prefix VIDs (`"user_100"`) to avoid collisions. `hash()` converts string→INT64 but risks collisions.

## Tag Design

- One tag per concept: `person(name, age)`, `player(jersey, position)`
- Multi-tag vertices for shared identity (person who is both player and coach)
- Avoid super-tags with 50+ properties — split into logical groups
- Use `NOT NULL` for required fields, `DEFAULT` for sensible defaults, `TTL_DURATION`+`TTL_COL` for auto-expiry

## Edge Type Design

- **Direction**: Model by most common traversal. Use `REVERSELY`/undirected MATCH for reverse queries
- **Rank**: `0` = one edge per src→dst; timestamp/sequence rank = parallel edges
- Keep edge properties lightweight; heavy data belongs on vertices
- Avoid edge indexes unless `LOOKUP ON edge_type` is needed (high write cost)

## Property Design

Use narrowest type that fits. `NOT NULL` prevents insertion without value. `DEFAULT` evaluated at insert time. TTL: `TTL_COL` + `TTL_DURATION`; cleaned during compaction.

**TTL timing**: Expired data is not deleted immediately. Removal happens during storage compaction — there can be a delay of minutes to hours depending on compaction schedule. Queries may still return TTL-expired data until compaction runs. Do not rely on TTL for real-time expiry; use explicit `DELETE` for immediate removal.

## Partition & Replica

Set at creation, **cannot change**. `partition_num`: `20 × disks_in_cluster` (20 for dev). `replica_factor`: odd number, 1 dev / 3 prod, needs ≥ replica_factor storage nodes. Run `BALANCE LEADER` after adding nodes.

## Index Strategy

**When**: Must index for `MATCH (v:tag)` or `LOOKUP`. Should index frequently filtered properties. Avoid for rarely queried or high-write-volume data.

| Type | Syntax | Use Case |
|------|--------|----------|
| Tag-level | `CREATE TAG INDEX idx ON tag()` | MATCH by tag |
| Single prop | `CREATE TAG INDEX idx ON tag(prop)` | Property filter |
| Composite | `CREATE TAG INDEX idx ON tag(p1, p2)` | Multi-prop filter (leftmost prefix) |
| Full-text | Elasticsearch integration | Text search |

**Lifecycle**: CREATE → REBUILD → check `SHOW TAG INDEX STATUS` (wait for FINISHED). For bulk import: import data first, then create + rebuild indexes.

## Common Modeling Patterns

- **Hierarchical**: Self-referencing edges (`:manages`, `:child_of`), query with variable-length paths
- **Many-to-many**: Model as edges directly; add intermediate vertex only if relationship has complex properties
- **Temporal**: Edge rank as timestamp, or `valid_from`/`valid_to` properties, or TTL
- **Multi-type vertices**: VID prefixes + separate tags in one space
- **Property vs vertex**: If value is shared by many vertices with own attributes → vertex; simple attribute → property
- **Super-vertices** (millions of edges): Partition by time, limit traversal depth, application fan-out

## Performance Tips

- **GO vs MATCH**: GO faster for known-VID traversals; MATCH for pattern matching / ad-hoc
- **LIMIT**: Always limit exploratory queries
- **WITH PROP**: Omit on `FIND PATH`/`GET SUBGRAPH` if only topology needed
- **Batch INSERT**: 100-200 rows per statement, avoid single-row loops
- **EXPLAIN/PROFILE**: EXPLAIN for plan, PROFILE for execution metrics
- **Compaction**: Trigger after bulk import/deletion to reclaim space
