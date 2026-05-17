# Built-in Functions Reference

> Complete function signatures for nGQL. For function name overview, see `ngql-syntax.md`. Official docs: https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/6.functions-and-expressions/1.math/

## Math Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `abs()` | `abs(x)` | Absolute value |
| `floor()` | `floor(x)` | Largest integer <= x |
| `ceil()` | `ceil(x)` | Smallest integer >= x |
| `round()` | `round(x, precision?, mode?)` | Round; mode: DOWN, CEILING, FLOOR, HALF_UP, etc. |
| `sqrt()` | `sqrt(x)` | Square root |
| `cbrt()` | `cbrt(x)` | Cube root |
| `hypot()` | `hypot(x, y)` | Hypotenuse: sqrt(x^2 + y^2) |
| `pow()` | `pow(x, y)` | x raised to power y |
| `exp()` | `exp(x)` | e^x |
| `exp2()` | `exp2(x)` | 2^x |
| `log()` | `log(x)` | Natural log (ln) |
| `log2()` | `log2(x)` | Base-2 log |
| `log10()` | `log10(x)` | Base-10 log |
| `sin/cos/tan()` | `sin(x)` | Trigonometric functions |
| `asin/acos/atan()` | `asin(x)` | Inverse trigonometric |
| `rand()` | `rand()` | Random float in [0, 1) |
| `rand32()` | `rand32(min?, max?)` | Random 32-bit int in [min, max) |
| `rand64()` | `rand64(min?, max?)` | Random 64-bit int in [min, max) |
| `bit_and/or/xor()` | `bit_and(a, b)` | Bitwise operations |
| `sign()` | `sign(x)` | Returns -1, 0, or 1 |
| `e()` | `e()` | Constant e (2.718...) |
| `pi()` | `pi()` | Constant pi (3.14159...) |
| `radians()` | `radians(degrees)` | Degrees to radians |
| `size()` | `size(list_or_string)` | Element count or string length |
| `range()` | `range(start, end, step?)` | Integer list [start, end] with step |

## String Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `lower/toLower()` | `lower(s)` | Lowercase |
| `upper/toUpper()` | `upper(s)` | Uppercase |
| `length()` | `length(s_or_path)` | Byte length (string) or hop count (path) |
| `trim()` | `trim(s)` | Remove leading/trailing spaces |
| `ltrim/rtrim()` | `ltrim(s)` | Remove leading/trailing spaces |
| `left()` | `left(s, n)` | First n characters |
| `right()` | `right(s, n)` | Last n characters |
| `lpad()` | `lpad(s, len, pad)` | Pad at head to length |
| `rpad()` | `rpad(s, len, pad)` | Pad at tail to length |
| `substr/substring()` | `substr(s, pos, len)` | Extract substring (pos starts at 1) |
| `reverse()` | `reverse(s)` | Reverse string |
| `replace()` | `replace(s, old, new)` | Replace all occurrences |
| `split()` | `split(s, delimiter)` | Split into list |
| `concat()` | `concat(s1, s2, ...)` | Concatenate (returns NULL if any arg is NULL) |
| `concat_ws()` | `concat_ws(sep, s1, s2, ...)` | Concatenate with separator (ignores NULL strings) |
| `strcasecmp()` | `strcasecmp(a, b)` | Case-insensitive comparison (returns int) |
| `extract()` | `extract(s, regex)` | Extract regex matches, returns list |
| `json_extract()` | `json_extract(json_str)` | Parse JSON string to map |

## Date/Time Functions

| Function | Signature | Returns | Description |
|----------|-----------|---------|-------------|
| `now()` | `now()` | int | Current Unix timestamp |
| `timestamp()` | `timestamp()` | timestamp | Current timestamp |
| `date()` | `date()` or `date("2023-01-01")` | date | Current or specified date (UTC) |
| `time()` | `time()` or `time("12:30:00")` | time | Current or specified time (UTC) |
| `datetime()` | `datetime()` or `datetime("2023-01-01T12:00:00")` | datetime | Current or specified datetime (UTC) |
| `duration()` | `duration({years: 1, months: 2})` | duration | Time duration for arithmetic |

Date accessors: `.year`, `.month`, `.day`, `.hour`, `.minute`, `.second`.

```ngql
RETURN date().year, datetime().month;
RETURN datetime("2023-06-15T10:30:00").hour;  -- 10
```

## Aggregation Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `COUNT()` | `COUNT(expr)` or `COUNT(*)` | Count rows/non-null values |
| `SUM()` | `SUM(expr)` | Sum of values |
| `AVG()` | `AVG(expr)` | Average value |
| `MIN()` | `MIN(expr)` | Minimum value |
| `MAX()` | `MAX(expr)` | Maximum value |
| `COLLECT()` | `COLLECT(expr)` | Aggregate into list |
| `STD()` | `STD(expr)` | Population standard deviation |

Use `COLLECT(expr)` + `toSet()` for distinct collection: `toSet(COLLECT(v.person.name))`.

## List Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `size()` | `size(list)` | Element count (includes NULL) |
| `range()` | `range(start, end, step?)` | Generate integer list |
| `head()` | `head(list)` | First element |
| `last()` | `last(list)` | Last element |
| `tail()` | `tail(list)` | All except first element |
| `reverse()` | `reverse(list)` | Reverse list |
| `reduce()` | `reduce(acc = init, x IN list \| expr)` | Fold/accumulate |
| `keys()` | `keys(vertex_or_edge)` | Property names as list (nGQL) |
| `labels()` | `labels(vertex)` | Tag list of vertex (nGQL) |
| `nodes()` | `nodes(path)` | Vertices in path (openCypher) |
| `relationships()` | `relationships(path)` | Edges in path (openCypher) |

List comprehension: `[x IN list WHERE condition | expression]`

```ngql
RETURN [x IN range(1,10) WHERE x % 2 == 0 | x * x] AS squares;
```

## Type Conversion Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `toBoolean()` | `toBoolean(val)` | Convert to bool |
| `toFloat()` | `toFloat(val)` | Convert to float. `toFloat("1e3")` = 1000.0 |
| `toInteger()` | `toInteger(val)` | Convert to int. `toInteger("1e3")` = 1000 |
| `toString()` | `toString(val)` | Convert any non-composite type to string |
| `toSet()` | `toSet(list)` | Deduplicate list into set |
| `hash()` | `hash(val)` | MurmurHash2 hash, returns int64 |

## Schema (Graph) Functions

| Function | Description | Context |
|----------|-------------|---------|
| `id(vertex)` | Vertex ID | Both |
| `properties(v_or_e)` | All properties as map | Both |
| `tags(vertex)` / `labels(vertex)` | Tag list | Both |
| `type(edge)` | Edge type name (string) | Both |
| `typeid(edge)` | Edge type internal ID (positive=forward, negative=reverse) | Both |
| `src(edge)` | Source vertex ID | Both |
| `dst(edge)` | Destination vertex ID | Both |
| `rank(edge)` | Edge rank (int) | Both |
| `vertices(path)` | All vertices in path | nGQL |
| `edges(path)` | All edges in path | nGQL |
| `length(path)` | Path hop count | Both |
| `nodes(path)` | Vertices in path | openCypher |
| `relationships(path)` | Edges in path | openCypher |
| `startNode(path)` | Starting vertex | openCypher |
| `endNode(path)` | Ending vertex | openCypher |
| `vertex` | Full vertex (use `AS alias`) | nGQL |
| `edge` | Full edge (use `AS alias`) | nGQL |

## Predicate Functions

Return `true` or `false`, typically used in `WHERE`.

| Function | Description |
|----------|-------------|
| `exists(prop)` | True if property exists on vertex/edge/map |
| `any(x IN list WHERE cond)` | True if at least one element matches |
| `all(x IN list WHERE cond)` | True if every element matches |
| `none(x IN list WHERE cond)` | True if no element matches |
| `single(x IN list WHERE cond)` | True if exactly one element matches |

```ngql
-- Filter variable-length edges
MATCH (a)-[e:follows*1..3]->(b) WHERE id(a) == "p1"
  AND ALL(x IN e WHERE x.weight > 0.5) RETURN b;
```

## Conditional Expressions

```ngql
-- CASE simple form
RETURN CASE v.person.age WHEN 30 THEN "thirty" WHEN 40 THEN "forty" ELSE "other" END;

-- CASE searched form (general conditions)
RETURN CASE WHEN v.person.age >= 60 THEN "senior" WHEN v.person.age >= 18 THEN "adult" ELSE "minor" END;

-- coalesce: returns first non-null argument
RETURN coalesce(v.person.nickname, v.person.name, "unknown");
```
