# Copyright (c) 2024 vesoft inc. All rights reserved.
#
# This source code is licensed under Apache 2.0 License.

nebula_add_library(
    sync_listener_obj OBJECT
    DrainerClient.cpp
    SyncListener.cpp
    SyncListenerFlags.cpp
    WalRetentionHook.cpp
)
