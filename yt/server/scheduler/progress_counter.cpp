#include "stdafx.h"
#include "progress_counter.h"
#include "private.h"

#include <ytlib/ytree/fluent.h>

namespace NYT {
namespace NScheduler {

using namespace NYTree;

////////////////////////////////////////////////////////////////////

TProgressCounter::TProgressCounter()
    : TotalEnabled(false)
    , Total_(0)
    , Running_(0)
    , Completed_(0)
    , Pending_(0)
    , Failed_(0)
    , Aborted_(0)
    , Lost_(0)
{ }

TProgressCounter::TProgressCounter(i64 total)
{
    Set(total);
}

void TProgressCounter::Set(i64 total)
{
    TotalEnabled = true;
    Running_ = 0;
    Completed_ = 0;
    Pending_ = total;
    Failed_ = 0;
    Aborted_ = 0;
    Lost_ = 0;
}

bool TProgressCounter::IsTotalEnabled() const
{
    return TotalEnabled;
}

void TProgressCounter::Increment(i64 value)
{
    YCHECK(TotalEnabled);
    Total_ += value;
    Pending_ += value;
}

i64 TProgressCounter::GetTotal() const
{
    YCHECK(TotalEnabled);
    return Total_;
}

i64 TProgressCounter::GetRunning() const
{
    return Running_;
}

i64 TProgressCounter::GetCompleted() const
{
    return Completed_;
}

i64 TProgressCounter::GetPending() const
{
    YCHECK(TotalEnabled);
    return Pending_;
}

i64 TProgressCounter::GetFailed() const
{
    return Failed_;
}

i64 TProgressCounter::GetAborted() const
{
    return Aborted_;
}

i64 TProgressCounter::GetLost() const
{
    return Lost_;
}

void TProgressCounter::Start(i64 count)
{
    if (TotalEnabled) {
        YCHECK(Pending_ >= count);
        Pending_ -= count;
    }
    Running_ += count;
}

void TProgressCounter::Completed(i64 count)
{
    YCHECK(Running_ >= count);
    Running_ -= count;
    Completed_ += count;
}

void TProgressCounter::Failed(i64 count)
{
    YCHECK(Running_ >= count);
    Running_ -= count;
    Failed_ += count;
    if (TotalEnabled) {
        Pending_ += count;
    }
}

void TProgressCounter::Aborted(i64 count)
{
    YCHECK(Running_ >= count);
    Running_ -= count;
    Aborted_ += count;
    if (TotalEnabled) {
        Pending_ += count;
    }
}

void TProgressCounter::Lost(i64 count)
{
    YCHECK(Completed_ >= count);
    Completed_ -= count;
    Lost_ += count;
    if (TotalEnabled) {
        Pending_ += count;
    }
}

void TProgressCounter::Finalize()
{
    if (TotalEnabled) {
        Total_ = Completed_;
        Pending_ = 0;
        Running_ = 0;
    }
}

void Serialize(const TProgressCounter& counter, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .DoIf(counter.IsTotalEnabled(), [&] (TFluentMap fluent) {
                fluent
                    .Item("total").Scalar(counter.GetTotal())
                    .Item("pending").Scalar(counter.GetPending());
            })
            .Item("running").Scalar(counter.GetRunning())
            .Item("completed").Scalar(counter.GetCompleted())
            .Item("failed").Scalar(counter.GetFailed())
            .Item("aborted").Scalar(counter.GetAborted())
            .Item("lost").Scalar(counter.GetLost())
        .EndMap();
}

Stroka ToString(const TProgressCounter& counter)
{
    return
        counter.IsTotalEnabled()
        ? Sprintf("T: %" PRId64 ", R: %" PRId64 ", C: %" PRId64 ", P: %" PRId64 ", F: %" PRId64 ", A: %" PRId64 ", L: %d" PRId64,
            counter.GetTotal(),
            counter.GetRunning(),
            counter.GetCompleted(),
            counter.GetPending(),
            counter.GetFailed(),
            counter.GetAborted())
        : Sprintf("R: %" PRId64 ", C: %" PRId64 ", F: %" PRId64 ", A: %" PRId64 ", L: %" PRId64,
            counter.GetRunning(),
            counter.GetCompleted(),
            counter.GetFailed(),
            counter.GetAborted());
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

