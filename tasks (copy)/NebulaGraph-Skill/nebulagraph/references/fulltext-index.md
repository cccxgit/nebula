# Full-Text Index Reference

> Full-text search via Elasticsearch integration. Official docs: https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/15.full-text-index-statements/1.search-with-text-based-index/

## Prerequisites

1. Deploy Elasticsearch cluster
2. Deploy and start Raft Listener on Storage nodes
3. Full-text indexes work on STRING/FIXED_STRING properties only

For deployment details, see: https://docs.nebula-graph.com.cn/3.8.0/4.deployment-and-installation/6.deploy-text-based-index/2.deploy-es/

## DDL

```ngql
-- Create full-text index (supports composite across multiple properties)
CREATE FULLTEXT TAG INDEX ft_person ON person(name, bio) ANALYZER="standard";
CREATE FULLTEXT EDGE INDEX ft_follows ON follows(note);

-- Show / Drop
SHOW FULLTEXT INDEXES;
DROP FULLTEXT INDEX ft_person;

-- Rebuild (required after creation)
REBUILD FULLTEXT INDEX;
```

For large datasets, set `snapshot_send_files=false` in `nebula-storaged.conf` before rebuild.

## Query Syntax

```ngql
LOOKUP ON <tag_or_edge> WHERE ES_QUERY(<index_name>, "<query_text>")
YIELD <return_list> [| LIMIT [offset,] count];
```

`score()` returns Lucene relevance score (higher = better match).

## Search Patterns

```ngql
-- Prefix search
LOOKUP ON person WHERE ES_QUERY(ft_person, "Chris") YIELD id(vertex), score();

-- Wildcard
LOOKUP ON person WHERE ES_QUERY(ft_person, "Da*") YIELD id(vertex);
LOOKUP ON person WHERE ES_QUERY(ft_person, "*son*") YIELD id(vertex);

-- Weighted search
LOOKUP ON person WHERE ES_QUERY(ft_person, "*son*^4 OR *ris*") YIELD id(vertex), score();

-- Property-specific search (multi-property indexes)
LOOKUP ON person WHERE ES_QUERY(ft_person, "name:Tim AND bio:engineer") YIELD id(vertex);
```

## Key Limitations

- Only works with LOOKUP, not MATCH or GO
- Requires Elasticsearch external deployment
- Custom analyzers (e.g., IK for Chinese) must be pre-installed in ES
- Native indexes and full-text indexes serve different purposes — native for exact/range, full-text for text search
