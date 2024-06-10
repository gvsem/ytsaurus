LIBRARY()

SRCS(
    callback_context.h
    executor_impl.h
    executor_impl.cpp
    log_lazy.h
    retry_policy.cpp
    trace_lazy.h
)

PEERDIR(
    contrib/ydb/public/sdk/cpp/client/ydb_topic/include

    contrib/ydb/public/sdk/cpp/client/ydb_common_client/impl
    contrib/ydb/public/sdk/cpp/client/ydb_types

    library/cpp/monlib/dynamic_counters
    library/cpp/retry
)

END()
