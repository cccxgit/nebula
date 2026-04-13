# Copyright (c) 2026 vesoft inc. All rights reserved.
#
# This source code is licensed under Apache 2.0 License.

nebula_add_library(
  meta_health_obj
  OBJECT
  MetaHealthState.cpp
  MetaSemanticProbeClient.cpp
  MetaSemanticHealthManager.cpp
)

nebula_add_subdirectory(test)
