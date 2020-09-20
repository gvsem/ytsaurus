#pragma once
#ifndef NONBLOCKING_BATCH_INL_H_
#error "Direct inclusion of this file is not allowed, include nonblocking_batch.h"
// For the sake of sane code completion.
#include "nonblocking_batch.h"
#endif
#undef NONBLOCKING_BATCH_INL_H_

#include <yt/core/concurrency/delayed_executor.h>

namespace NYT::NConcurrency {

////////////////////////////////////////////////////////////////////////////////

template <class T>
TNonblockingBatch<T>::TNonblockingBatch(int maxBatchSize, TDuration batchDuration)
    : MaxBatchSize_(maxBatchSize)
    , BatchDuration_(batchDuration)
{ }

template <class T>
TNonblockingBatch<T>::~TNonblockingBatch()
{
    TGuard<TAdaptiveLock> guard(SpinLock_);
    ResetTimer(guard);
}

template <class T>
template <class... U>
void TNonblockingBatch<T>::Enqueue(U&& ... u)
{
    TGuard<TAdaptiveLock> guard(SpinLock_);
    CurrentBatch_.emplace_back(std::forward<U>(u)...);
    StartTimer(guard);
    CheckFlush(guard);
}

template <class T>
TFuture<typename TNonblockingBatch<T>::TBatch> TNonblockingBatch<T>::DequeueBatch()
{
    TGuard<TAdaptiveLock> guard(SpinLock_);
    auto promise = NewPromise<TBatch>();
    Promises_.push_back(promise);
    StartTimer(guard);
    CheckReturn(guard);
    return promise.ToFuture();
}

template <class T>
void TNonblockingBatch<T>::Drop()
{
    std::queue<TBatch> batches;
    std::deque<TPromise<TBatch>> promises;
    {
        TGuard<TAdaptiveLock> guard(SpinLock_);
        Batches_.swap(batches);
        Promises_.swap(promises);
        CurrentBatch_.clear();
        ResetTimer(guard);
    }
    for (auto&& promise : promises) {
        promise.Set(TBatch{});
    }
}

template <class T>
void TNonblockingBatch<T>::ResetTimer(TGuard<TAdaptiveLock>& guard)
{
    if (TimerState_ == ETimerState::Started) {
        ++FlushGeneration_;
        TDelayedExecutor::CancelAndClear(BatchFlushCookie_);
    }
    TimerState_ = ETimerState::Initial;
}

template <class T>
void TNonblockingBatch<T>::StartTimer(TGuard<TAdaptiveLock>& guard)
{
    if (TimerState_ == ETimerState::Initial && !Promises_.empty() && !CurrentBatch_.empty()) {
        TimerState_ = ETimerState::Started;
        BatchFlushCookie_ = TDelayedExecutor::Submit(
            BIND(&TNonblockingBatch::OnBatchTimeout, MakeWeak(this), FlushGeneration_),
            BatchDuration_);
    }
}

template <class T>
bool TNonblockingBatch<T>::IsFlushNeeded(TGuard<TAdaptiveLock>& guard) const
{
    return
        static_cast<int>(CurrentBatch_.size()) == MaxBatchSize_ ||
        TimerState_ == ETimerState::Finished;
}

template <class T>
void TNonblockingBatch<T>::CheckFlush(TGuard<TAdaptiveLock>& guard)
{
    if (!IsFlushNeeded(guard)) {
        return;
    }
    ResetTimer(guard);
    Batches_.push(std::move(CurrentBatch_));
    CurrentBatch_.clear();
    CheckReturn(guard);
}

template <class T>
void TNonblockingBatch<T>::CheckReturn(TGuard<TAdaptiveLock>& guard)
{
    if (Promises_.empty() || Batches_.empty()) {
        return;
    }
    auto batch = std::move(Batches_.front());
    Batches_.pop();
    auto promise = std::move(Promises_.front());
    Promises_.pop_front();
    guard.Release();
    promise.Set(std::move(batch));
}

template <class T>
void TNonblockingBatch<T>::OnBatchTimeout(ui64 generation)
{
    TGuard<TAdaptiveLock> guard(SpinLock_);
    if (generation != FlushGeneration_) {
        // Chunk had been prepared.
        return;
    }
    TimerState_ = ETimerState::Finished;
    CheckFlush(guard);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency
