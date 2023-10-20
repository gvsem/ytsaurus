GTEST(unittester-query)

INCLUDE(${ARCADIA_ROOT}/yt/ya_cpp.make.inc)

ALLOCATOR(TCMALLOC)

SRCS(
    ast_ut.cpp
    ql_computed_columns_ut.cpp
    ql_expressions_ut.cpp
    ql_helpers.cpp
    ql_misc_ut.cpp
    ql_query_ut.cpp
    ql_range_inference_ut.cpp
    ql_range_coordination_ut.cpp
)

ADDINCL(
    contrib/libs/sparsehash/src
)

# This flag is required for linking code of bc functions.
# Note that -rdynamic enabled by default in arcadia build, but we do not want to rely on it.
LDFLAGS(-rdynamic)

INCLUDE(${ARCADIA_ROOT}/yt/opensource_tests.inc)

PEERDIR(
    yt/yt/build
    yt/yt/core/test_framework
    yt/yt/library/query/engine
    yt/yt/library/query/engine_api
    yt/yt/library/query/unittests/udf
    contrib/libs/sparsehash
)

SIZE(MEDIUM)

END()
