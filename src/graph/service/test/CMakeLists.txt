# Copyright (c) 2021 vesoft inc. All rights reserved.
#
# This source code is licensed under Apache 2.0 License.

# note: These flags is used for standalone version ut.

nebula_add_library(
    sa_test_graph_flags_obj OBJECT
    StandAloneTestGraphFlags.cpp
)

nebula_add_test(
    NAME slow_query_logger_test
    SOURCES
        SlowQueryLoggerTest.cpp
        ../SlowQueryLogger.cpp
    OBJECTS
        $<TARGET_OBJECTS:base_obj>
    LIBRARIES
        gtest
        gtest_main
)

nebula_add_test(
    NAME running_slow_query_logger_test
    SOURCES
        RunningSlowQueryLoggerTest.cpp
        ../RunningSlowQueryLogger.cpp
    OBJECTS
        $<TARGET_OBJECTS:base_obj>
    LIBRARIES
        gtest
        gtest_main
)
