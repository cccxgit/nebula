# Composite Queries & Variables

> Variable scoping, property references, and query composition in nGQL. Official docs: https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/4.variable-and-composite-queries/1.composite-queries/

## Three Composition Methods

| Method | Syntax | Context |
|--------|--------|---------|
| openCypher clauses | `MATCH ... WITH ... RETURN` | openCypher |
| Semicolons | `stmt1; stmt2;` | nGQL (returns last result only) |
| Pipe | `stmt1 \| stmt2` | nGQL (passes result set forward) |

**Never mix openCypher and native nGQL** in one composite query. `MATCH ... | GO ...` is invalid.

## Pipe Operator `|`

Passes the output of one statement as input to the next.

```ngql
GO FROM "p1" OVER follows YIELD dst(edge) AS id
| GO FROM $-.id OVER serves YIELD $-.id, dst(edge) AS team;
```

## Property Reference Symbols

| Symbol | Meaning | Context |
|--------|---------|---------|
| `$-.col` | Column from piped result | Pipe queries |
| `$^.tag.prop` | Source vertex property | GO |
| `$$.tag.prop` | Destination vertex property | GO |

```ngql
GO FROM "p1" OVER follows
YIELD $^.person.name AS src_name, $$.person.name AS dst_name, follows.weight;
```

**Note**: `$^` and `$$` only work in native nGQL (GO), not in MATCH.

## User-Defined Variables

Native nGQL supports `$var_name` variables for storing intermediate results.

```ngql
$friends = GO FROM "p1" OVER follows YIELD dst(edge) AS id;
GO FROM $friends.id OVER serves YIELD $friends.id, dst(edge) AS team;
```

- Variable names: letters, numbers, underscores (case-sensitive)
- Scope: valid only within the current composite query
- End variable assignment statements with `;`

## Edge Built-in Properties

Available in GO statements:

| Property | Description |
|----------|-------------|
| `_src` | Edge source vertex ID |
| `_dst` | Edge destination vertex ID |
| `_type` | Edge type ID (positive = forward, negative = reverse) |
| `_rank` | Edge rank value |

```ngql
GO FROM "p1" OVER follows YIELD follows._src, follows._dst, follows._rank;
```

## Transaction Warning

Composite queries have **no transaction support**. If any sub-statement fails, previous results are not rolled back.
