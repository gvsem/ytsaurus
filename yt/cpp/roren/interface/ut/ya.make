GTEST()

SRCS(
    co_gbk_result_ut.cpp
    coder_ut.cpp
    dump_ut.cpp
    input_ut.cpp
    transform_names_ut.cpp
)

PEERDIR(
    yt/cpp/roren/bigrt
    yt/cpp/roren/interface
    yt/cpp/roren/interface/ut/proto
)

END()
