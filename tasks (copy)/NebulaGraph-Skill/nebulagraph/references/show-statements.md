# SHOW Statements Reference

> All SHOW statement variants in nGQL. Official docs: https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/7.general-query-statements/6.show/1.show-charset/

## Space & Schema

```ngql
SHOW SPACES;                          -- List all spaces
SHOW CREATE SPACE <space_name>;       -- Show CREATE statement for a space
DESCRIBE SPACE <space_name>;          -- Space details (vid_type, partition, replica)

SHOW TAGS;                            -- List tags in current space
SHOW EDGES;                           -- List edge types in current space
SHOW CREATE TAG <tag_name>;           -- Show CREATE statement for a tag
SHOW CREATE EDGE <edge_name>;         -- Show CREATE statement for an edge
DESCRIBE TAG <tag_name>;              -- Tag property details
DESCRIBE EDGE <edge_name>;            -- Edge property details
```

## Index

```ngql
SHOW TAG INDEXES;                     -- List tag indexes
SHOW EDGE INDEXES;                    -- List edge indexes
SHOW TAG INDEX STATUS;                -- Rebuild status (wait for FINISHED)
SHOW EDGE INDEX STATUS;               -- Rebuild status
SHOW CREATE TAG INDEX <index_name>;   -- Show CREATE statement for index
DESCRIBE TAG INDEX <index_name>;      -- Index details
```

## Cluster & System

```ngql
SHOW HOSTS;                           -- Storage hosts, partitions, leader count
SHOW HOSTS GRAPH;                     -- Graph service hosts
SHOW HOSTS META;                      -- Meta service hosts
SHOW HOSTS STORAGE;                   -- Storage service hosts

SHOW PARTS;                           -- Partition distribution in current space
SHOW PARTS <part_id>;                 -- Specific partition details

SHOW CHARSET;                         -- Supported character sets
SHOW COLLATION;                       -- Supported collations

SHOW META LEADER;                     -- Current Meta leader host
```

## Statistics

```ngql
SUBMIT JOB STATS;                     -- Must run first to collect stats
SHOW STATS;                           -- Vertex/edge counts per tag/type
```

**Note**: `SHOW STATS` returns stale data until `SUBMIT JOB STATS` completes.

## Users & Sessions

```ngql
SHOW USERS;                           -- List all users
SHOW ROLES IN <space_name>;           -- User roles in a space
SHOW SNAPSHOTS;                       -- List all snapshots
SHOW SESSIONS;                        -- Active sessions
SHOW QUERIES;                         -- Currently running queries
```

## Jobs

```ngql
SHOW JOBS;                            -- All unexpired jobs in current space
SHOW JOB <job_id>;                    -- Specific job details and tasks
```
