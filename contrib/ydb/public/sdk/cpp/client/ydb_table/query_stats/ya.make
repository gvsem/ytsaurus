LIBRARY()

SRCS(
    stats.cpp
)

PEERDIR(
    contrib/libs/protobuf
    contrib/ydb/public/api/protos
    contrib/ydb/public/sdk/cpp/client/ydb_query
)

END()
