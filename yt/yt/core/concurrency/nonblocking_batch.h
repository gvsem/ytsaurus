#pragma once

#include <yt/core/actions/future.h>

#include <queue>
#include <vector>

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(ETNonblockingBatchTimerState,
    (Initial)
    (Started)
    (Finished)
);

//! Nonblocking MPMC queue that supports batching.
/*!
 * TNonblockingBatch accepts 2 parameters:
 * - batchElements is maximum number of elements to be placed inside batch.
 * - batchDuration is a time period to create the batch.
 * If producer exceeds batchDuration the consumer receives awaited batch.
 * If there is no consumer thus the batch will be limited by batchElements.
 */
template <class T>
class TNonblockingBatch
    : public TRefCounted
{
public:
    using TBatch = std::vector<T>;

    TNonblockingBatch(int maxBatchSize, TDuration batchDuration);
    ~TNonblockingBatch();

    template <class... U>
    void Enqueue(U&& ... u);

    TFuture<TBatch> DequeueBatch();
    void Drop();

private:
    using ETimerState = ETNonblockingBatchTimerState;

    const int MaxBatchSize_;
    const TDuration BatchDuration_;

    TAdaptiveLock SpinLock_;
    TBatch CurrentBatch_;
    ETimerState TimerState_ = ETimerState::Initial;
    std::queue<TBatch> Batches_;
    std::deque<TPromise<TBatch>> Promises_;
    TDelayedExecutorCookie BatchFlushCookie_;
    ui64 FlushGeneration_ = 0;

    void ResetTimer(TGuard<TAdaptiveLock>& guard);
    void StartTimer(TGuard<TAdaptiveLock>& guard);
    bool IsFlushNeeded(TGuard<TAdaptiveLock>& guard) const;
    void CheckFlush(TGuard<TAdaptiveLock>& guard);
    void CheckReturn(TGuard<TAdaptiveLock>& guard);
    void OnBatchTimeout(ui64 generation);
};

template <class T>
using TNonblockingBatchPtr = TIntrusivePtr<TNonblockingBatch<T>>;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency

#define NONBLOCKING_BATCH_INL_H_
#include "nonblocking_batch-inl.h"
#undef NONBLOCKING_BATCH_INL_H_
