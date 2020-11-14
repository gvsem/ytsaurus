#include "invoker_queue.h"
#include "private.h"

#include <util/thread/lfqueue.h>

namespace NYT::NConcurrency {

using namespace NProfiling;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = ConcurrencyLogger;

////////////////////////////////////////////////////////////////////////////////

//! Queue interface to enqueue or dequeue actions.
struct IActionQueue
{
    virtual ~IActionQueue() = default;

    //! Inserts element into the queue.
    /*!
     * \param action Action to be enqueued.
     * \param index Index is used as a hint to place the action
     * using the most suitable implementation-specific way.
     */
    virtual void Enqueue(TEnqueuedAction&& action, TTscp tscp) = 0;

    //! Extracts single element from the queue.
    /*!
     * \param action Pointer to action instance to be dequeued.
     * \param index Index is used as a hint to extract the action
     * using the most suitable implementation-specific way.
     * \return |true| on successful operation. False on empty queue.
     */
    virtual bool Dequeue(TEnqueuedAction* action, TTscp tscp) = 0;
};

class TLockFreeActionQueue
    : public IActionQueue
{
public:
    virtual void Enqueue(TEnqueuedAction&& action, TTscp /*tscp*/) override
    {
        Queue_.Enqueue(std::move(action));
    }

    virtual bool Dequeue(TEnqueuedAction* action, TTscp /*tscp*/) override
    {
        return Queue_.Dequeue(action);
    }

private:
    TLockFreeQueue<TEnqueuedAction> Queue_;
};

template <typename T, typename TLock>
class TLockQueue
{
    using TLockGuard = TGuard<TLock>;
    using TTryLockGuard = TGuard<TLock, TTryLockOps<TLock>>;

public:
    bool Dequeue(T* val)
    {
        TLockGuard lock(Lock_);
        if (Queue_.empty()) {
            return false;
        }
        *val = std::move(Queue_.front());
        Queue_.pop_front();
        return true;
    }

    template <typename... U>
    void Enqueue(U&&... val)
    {
        TLockGuard lock(Lock_);
        Queue_.emplace_back(std::forward<U>(val)...);
    }

    bool TryDequeue(T* val)
    {
        TTryLockGuard lock(Lock_);
        if (!lock || Queue_.empty()) {
            return false;
        }
        *val = std::move(Queue_.front());
        Queue_.pop_front();
        return true;
    }

    template <typename... U>
    bool TryEnqueue(U&&... val)
    {
        TTryLockGuard lock(Lock_);
        if (!lock) {
            return false;
        }
        Queue_.emplace_back(std::forward<U>(val)...);
        return true;
    }

private:
    std::deque<T> Queue_;
    TLock Lock_;
};

template <typename T>
class TTryQueues
{
    using TQueueType = TLockQueue<T, TAdaptiveLock>;

public:
    void Configure(int queueCount)
    {
        Queues_.resize(queueCount);
    }

    template <typename U>
    void Enqueue(U&& val, TTscp tscp)
    {
        TryQueue(
            tscp,
            [&] (TQueueType& q) {
                return q.TryEnqueue(std::forward<U>(val));
            },
            [&] (TQueueType& q) {
                q.Enqueue(std::forward<U>(val));
                return true;
            });
    }

    bool Dequeue(T* val, TTscp tscp)
    {
        YT_ASSERT(val);

        return TryQueue(
            tscp,
            [&] (TQueueType& q) {
                return q.TryDequeue(val);
            },
            [&] (TQueueType& q) {
                return q.Dequeue(val);
            });
    }

private:
    TQueueType& GetQueue(int index)
    {
        return Queues_[index & (TTscp::MaxProcessorId - 1)];
    }

    template <typename FTry, typename F>
    bool TryQueue(TTscp tscp, FTry&& fTry, F&& f)
    {
        for (int shift = 0; shift < TTscp::MaxProcessorId; ++shift) {
            if (fTry(GetQueue(tscp.ProcessorId + shift))) {
                return true;
            }
        }
        return f(GetQueue(tscp.ProcessorId));
    }

    std::array<TQueueType, TTscp::MaxProcessorId> Queues_;
};

class TMultiLockActionQueue
    : public IActionQueue
{
public:
    virtual void Enqueue(TEnqueuedAction&& action, TTscp tscp) override
    {
        Queue_.Enqueue(action, tscp);
    }

    virtual bool Dequeue(TEnqueuedAction *action, TTscp tscp) override
    {
        return Queue_.Dequeue(action, tscp);
    }

private:
    TTryQueues<TEnqueuedAction> Queue_;
};

std::unique_ptr<IActionQueue> CreateActionQueue(EInvokerQueueType type)
{
    switch (type) {
        case EInvokerQueueType::SingleLockFreeQueue:
            return std::make_unique<TLockFreeActionQueue>();
        case EInvokerQueueType::MultiLockQueue:
            return std::make_unique<TMultiLockActionQueue>();
        default:
            YT_ABORT();
    }
}

////////////////////////////////////////////////////////////////////////////////

TInvokerQueue::TInvokerQueue(
    std::shared_ptr<TEventCount> callbackEventCount,
    const TTagSet& tags,
    bool enableLogging,
    bool enableProfiling,
    EInvokerQueueType type)
    : CallbackEventCount(std::move(callbackEventCount))
    , EnableLogging(enableLogging)
    , Queue(CreateActionQueue(type))
{
    if (enableProfiling) {
        auto profiler = TRegistry("/action_queue").WithTags(tags);

        EnqueuedCounter = profiler.Counter("/enqueued");
        DequeuedCounter = profiler.Counter("/dequeued");
        profiler.AddFuncGauge("/size", MakeStrong(this), [this] {
            return SizeGauge.load();
        });
        WaitTimer = profiler.Timer("/time/wait");
        ExecTimer = profiler.Timer("/time/exec");
        CumulativeTimeCounter = profiler.TimeCounter("/time/cumulative");
        TotalTimer = profiler.Timer("/time/total");
    }

    Y_UNUSED(EnableLogging);
}

TInvokerQueue::~TInvokerQueue() = default;

void TInvokerQueue::SetThreadId(TThreadId threadId)
{
    ThreadId = threadId;
}

void TInvokerQueue::Invoke(TClosure callback)
{
    YT_ASSERT(callback);

    if (!Running.load(std::memory_order_relaxed)) {
        YT_LOG_TRACE_IF(
            EnableLogging,
            "Queue had been shut down, incoming action ignored: %p",
            callback.GetHandle());
        return;
    }

    YT_LOG_TRACE_IF(EnableLogging, "Callback enqueued: %p",
        callback.GetHandle());

    auto tscp = TTscp::Get();

    SizeGauge += 1;
    EnqueuedCounter.Increment();

    TEnqueuedAction action;
    action.Finished = false;
    action.EnqueuedAt = tscp.Instant;
    action.Callback = std::move(callback);

    Queue->Enqueue(std::move(action), tscp);

    CallbackEventCount->NotifyOne();
}

#ifdef YT_ENABLE_THREAD_AFFINITY_CHECK
TThreadId TInvokerQueue::GetThreadId() const
{
    return ThreadId;
}

bool TInvokerQueue::CheckAffinity(const IInvokerPtr& invoker) const
{
    return invoker.Get() == this;
}
#endif

void TInvokerQueue::Shutdown()
{
    Running.store(false, std::memory_order_relaxed);
}

void TInvokerQueue::Drain()
{
    YT_VERIFY(!Running.load(std::memory_order_relaxed));

    Queue.reset();
    SizeGauge = 0;
}

TClosure TInvokerQueue::BeginExecute(TEnqueuedAction* action)
{
    YT_ASSERT(action && action->Finished);
    YT_ASSERT(Queue);

    auto tscp = TTscp::Get();
    if (!Queue->Dequeue(action, tscp)) {
        return {};
    }

    action->StartedAt = tscp.Instant;

    auto waitTime = CpuDurationToDuration(action->StartedAt - action->EnqueuedAt);

    DequeuedCounter.Increment();
    WaitTimer.Record(waitTime);

    SetCurrentInvoker(this);

    return std::move(action->Callback);
}

void TInvokerQueue::EndExecute(TEnqueuedAction* action)
{
    SetCurrentInvoker(nullptr);

    YT_ASSERT(action);

    if (action->Finished) {
        return;
    }

    auto tscp = TTscp::Get();
    action->FinishedAt = tscp.Instant;
    action->Finished = true;

    auto timeFromStart = CpuDurationToDuration(action->FinishedAt - action->StartedAt);
    auto timeFromEnqueue = CpuDurationToDuration(action->FinishedAt - action->EnqueuedAt);

    SizeGauge -= 1;
    ExecTimer.Record(timeFromStart);
    CumulativeTimeCounter.Add(timeFromStart);
    TotalTimer.Record(timeFromEnqueue);
}

int TInvokerQueue::GetSize() const
{
    return SizeGauge;
}

bool TInvokerQueue::IsEmpty() const
{
    return GetSize() == 0;
}

bool TInvokerQueue::IsRunning() const
{
    return Running.load(std::memory_order_relaxed);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NConcurrency
