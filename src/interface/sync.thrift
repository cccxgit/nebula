/* Copyright (c) 2018 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

namespace cpp nebula.sync.cpp2
namespace java com.vesoft.nebula.sync
namespace py nebula3.sync

include "common.thrift"

enum LogOp {
    UNKNOWN          = 0,
    OP_PUT           = 1,
    OP_MULTI_PUT     = 2,
    OP_REMOVE        = 3,
    OP_MULTI_REMOVE  = 4,
    OP_REMOVE_RANGE  = 5,
    OP_BATCH_WRITE   = 6,
    OP_META_CMD      = 100,
}

enum SnapshotPhase {
    NOT_IN_SNAPSHOT = 0,
    BEGIN           = 1,
    DATA            = 2,
    END             = 3,
}

struct SyncLogEntry {
    1: required common.LogID     logId,
    2: required common.TermID    termId,
    3: required i64              timestamp,    // us since epoch at primary commit
    4: required LogOp            op,
    5: required binary           payload,      // raw encoded body
    6: optional i64              schemaVer,    // for DML; required when op != OP_META_CMD
    7: optional binary           hlc,          // 16B HLC, for future bidirectional
}

struct SyncLogBatch {
    1: required common.GraphSpaceID   spaceId,
    2: required common.PartitionID    partId,
    3: required i64                   clusterId,      // origin cluster, loop guard
    4: required i64                   epoch,          // primary epoch when emitted
    5: required common.LogID          firstLogId,
    6: required common.LogID          lastLogId,
    7: required list<SyncLogEntry>    entries,
    8: optional binary                compressed,     // when set, entries is empty, batch is zstd compressed
    9: optional SnapshotPhase         snapshotPhase = SnapshotPhase.NOT_IN_SNAPSHOT,
    10: optional list<i64>            forwarders,     // chain of clusterIds traversed
}

struct AppendLogsRequest {
    1: required SyncLogBatch  batch,
    2: required string        listenerToken,
}

struct AppendLogsResponse {
    1: required common.ErrorCode  code,
    2: optional common.LogID      ackedLogId,
    3: optional common.LogID      requestResendFrom,
}

struct HeartbeatRequest {
    1: required common.GraphSpaceID  spaceId,
    2: required common.PartitionID   partId,
    3: required common.LogID         primaryCommittedLogId,
    4: required i64                  primaryWallClockMs,
    5: required i64                  clusterId,
    6: required i64                  epoch,
}

struct HeartbeatResponse {
    1: required common.ErrorCode  code,
    2: required common.LogID      drainerLastAppliedLogId,
    3: required i64               drainerWallClockMs,
    4: optional bool              drainerWantsResnap,
}

struct CheckpointQueryRequest {
    1: required common.GraphSpaceID         spaceId,
    2: required list<common.PartitionID>    parts,
    3: required i64                         clusterId,
}

struct CheckpointQueryResponse {
    1: required common.ErrorCode                         code,
    2: required map<common.PartitionID, common.LogID>    lastApplied,
}

service SyncService {
    AppendLogsResponse appendLogs(1: AppendLogsRequest req),
    HeartbeatResponse heartbeat(1: HeartbeatRequest req),
    CheckpointQueryResponse queryCheckpoint(1: CheckpointQueryRequest req),
}
