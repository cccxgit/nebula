# Administration Reference

> JOB management, query termination, and user management in nGQL. Official docs: https://docs.nebula-graph.com.cn/3.8.0/3.ngql-guide/4.job-statements/

## JOB Management

All JOB commands require a graph space selected (`USE space`).

### Submit Jobs

```ngql
SUBMIT JOB COMPACT;         -- Trigger RocksDB compaction (current space)
SUBMIT JOB FLUSH;            -- Write memtable to disk (current space)
SUBMIT JOB STATS;            -- Collect statistics (view with SHOW STATS)
SUBMIT JOB BALANCE LEADER;   -- Rebalance leader partitions (all spaces)
```

SST file import:
```ngql
SUBMIT JOB DOWNLOAD HDFS "hdfs://host:9000/sst";
SUBMIT JOB INGEST;
```

### Monitor & Control Jobs

```ngql
SHOW JOBS;                   -- List all unexpired jobs (default expiry: 1 week)
SHOW JOB <job_id>;           -- Job details and task status
STOP JOB <job_id>;           -- Stop an incomplete job
RECOVER JOB;                 -- Retry all failed/stopped jobs
RECOVER JOB <job_id>;        -- Retry a specific job
```

### Job Status Lifecycle

`QUEUE` → `RUNNING` → `FINISHED` / `FAILED` / `STOPPED` → `REMOVED`

## Query Termination

### KILL QUERY

Terminates a running query. God role can kill any; others can only kill their own.

```ngql
-- First, find the query
SHOW QUERIES;

-- Then kill it
KILL QUERY(SESSION=<session_id>, PLAN=<plan_id>);
```

### KILL SESSION

```ngql
-- View active sessions
SHOW SESSIONS;

-- Kill a session
KILL SESSION <session_id>;
```

## User Management

```ngql
-- Create / Drop users
CREATE USER [IF NOT EXISTS] user1 WITH PASSWORD "pass123";
DROP USER [IF EXISTS] user1;
ALTER USER user1 WITH PASSWORD "newpass";

-- Roles: GOD, ADMIN, DBA, USER, GUEST
GRANT ROLE ADMIN ON space1 TO user1;
REVOKE ROLE ADMIN ON space1 FROM user1;

-- View
SHOW USERS;
SHOW ROLES IN space1;
CHANGE PASSWORD account FROM "old" TO "new";
```

### Role Permissions

| Role | Description |
|------|-------------|
| GOD | Super admin, all permissions on all spaces |
| ADMIN | Full permissions on specific space |
| DBA | Schema + data operations, no user management |
| USER | Read/write data, no schema changes |
| GUEST | Read-only |

For detailed permission matrix, see: https://docs.nebula-graph.com.cn/3.8.0/7.data-security/1.authentication/3.role-list/
