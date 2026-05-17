# Operators Reference

> Detailed operator reference for nGQL. For quick syntax, see `ngql-syntax.md`. Official docs: https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/5.operators/1.comparison/

## Comparison Operators

| Operator | Description |
|----------|-------------|
| `==` | Equal (not `=`) |
| `!=`, `<>` | Not equal |
| `<`, `<=`, `>`, `>=` | Comparison |
| `IS NULL` | Is null |
| `IS NOT NULL` | Is not null |
| `IS EMPTY` | Non-existence (nGQL only, not in MATCH) |
| `IS NOT EMPTY` | Exists |

```ngql
-- Type-sensitive: '2' == 2 returns false
-- Null: null == null returns NULL (not true)
-- Use IS NULL to test for null
MATCH (v:person) WHERE v.person.name IS NOT NULL RETURN v;
```

## Boolean Operators

| Operator | Description |
|----------|-------------|
| `AND` | Logical and |
| `OR` | Logical or |
| `NOT` | Logical not |
| `XOR` | Logical exclusive or |

Non-zero numbers cannot be implicitly converted to boolean.

## String Operators

| Operator | Description |
|----------|-------------|
| `CONTAINS` | Substring match (case-sensitive) |
| `STARTS WITH` | Prefix match |
| `NOT STARTS WITH` | Negated prefix match |
| `ENDS WITH` | Suffix match |
| `NOT ENDS WITH` | Negated suffix match |
| `=~` | Regex match (openCypher only: MATCH/WITH, not GO/LOOKUP/FETCH) |
| `+` | String concatenation |

```ngql
MATCH (v:person) WHERE v.person.name STARTS WITH "Tim" RETURN v;
MATCH (v:person) WHERE v.person.name =~ 'T.*' RETURN v;  -- regex, openCypher only
```

## Set Operators

Combine results from two queries. Column names and order must match.

| Operator | Description |
|----------|-------------|
| `UNION` / `UNION DISTINCT` | Union without duplicates |
| `UNION ALL` | Union with duplicates |
| `INTERSECT` | Intersection |
| `MINUS` | Difference (A - B) |

```ngql
GO FROM "p1" OVER follows YIELD dst(edge) AS id
UNION
GO FROM "p2" OVER follows YIELD dst(edge) AS id;

GO FROM "p1" OVER follows YIELD dst(edge) AS id
MINUS
GO FROM "p2" OVER follows YIELD dst(edge) AS id;
```

Pipe `|` has higher precedence than set operators. Use parentheses to override.

## List Operators

| Operator | Description |
|----------|-------------|
| `+` | Concatenate lists: `[1,2] + [3,4]` |
| `IN` | Membership test: `x IN [1,2,3]` |
| `NOT IN` | Negated membership |
| `[]` | Subscript access (0-indexed): `list[0]` |

```ngql
-- IN returns NULL when checking for NULL in lists
RETURN NULL IN [1, 2, NULL];  -- NULL
```

## Arithmetic Operators

`+`, `-`, `*`, `/`, `%` (modulo), `-` (unary negation).

## Operator Precedence (high to low)

1. `-` (unary negation)
2. `!`, `NOT`
3. `*`, `/`, `%`
4. `-`, `+`
5. `==`, `>=`, `>`, `<=`, `<`, `<>`, `!=`
6. `AND`
7. `OR`, `XOR`
8. `=` (assignment)

Same-level operators evaluate left-to-right, except assignment (right-to-left).

**nGQL vs openCypher difference**: `x < y <= z` evaluates as `(x < y) <= z` in nGQL, not `x < y AND y <= z`. May return NULL unexpectedly.
