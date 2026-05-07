# Copyright (c) 2024 vesoft inc. All rights reserved.
#
# This source code is licensed under Apache 2.0 License.

nebula_add_library(
    drainer_obj OBJECT
    DrainerFlags.cpp
    DrainerEnv.cpp
    DrainerService.cpp
    CheckpointStore.cpp
    BackupClientCache.cpp
    QueueManager.cpp
    PartApplier.cpp
    MetaApplier.cpp
)
