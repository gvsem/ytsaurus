#pragma once

#include <yt/core/profiling/profiler.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

//! A single-threaded pretty inefficient wrapper around profiler which
//! ensures that same value is not exported twice in a row.
class TCachingProfilerWrapper
{
public:
    explicit TCachingProfilerWrapper(const NProfiling::TProfiler* underlyingProfiler);

    //! Enqueues a new sample with tags.
    void Enqueue(
        const NYPath::TYPath& path,
        NProfiling::TValue value,
        NProfiling::EMetricType metricType,
        const NProfiling::TTagIdList& tagIds = {}) const;

private:
    using TKey = std::pair<NYPath::TYPath, NProfiling::TTagIdList>;

    struct TState
    {
        NProfiling::TValue Value;
        TInstant Instant;
    };

    mutable THashMap<TKey, TState> PreviousValues_;
    const NProfiling::TProfiler* UnderlyingProfiler_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
