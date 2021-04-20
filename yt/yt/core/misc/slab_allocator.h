#pragma once

#include "common.h"
#include "free_list.h"
#include "format.h"
#include "error.h"
#include "memory_usage_tracker.h"

#include <yt/yt/core/misc/atomic_ptr.h>

#include <yt/yt/library/profiling/sensor.h>

#include <yt/yt/core/profiling/profiler.h>

#include <array>

namespace NYT {

/////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TSmallArena)

class TLargeArena;

/////////////////////////////////////////////////////////////////////////////

class TSlabAllocator
{
public:
    explicit TSlabAllocator(
        const NProfiling::TProfiler& profiler = {},
        IMemoryUsageTrackerPtr memoryTracker = nullptr);

    void* Allocate(size_t size);
    static void Free(void* ptr);

    void ReallocateArenasIfNeeded();

private:
    const NProfiling::TProfiler Profiler_;

    struct TLargeArenaDeleter
    {
        void operator() (TLargeArena* arena);
    };

    using TLargeArenaPtr = std::unique_ptr<TLargeArena, TLargeArenaDeleter>;

    TAtomicPtr<TSmallArena> SmallArenas_[NYTAlloc::SmallRankCount];
    TLargeArenaPtr LargeArena_;
};

bool IsReallocationNeeded(const void* ptr);

/////////////////////////////////////////////////////////////////////////////

} // namespace NYT

