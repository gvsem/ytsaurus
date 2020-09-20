#include "traffic_meter.h"

namespace NYT::NChunkClient {

////////////////////////////////////////////////////////////////////////////////

TTrafficMeter::TTrafficMeter(const TDataCenterName& localDataCenter)
    : LocalDataCenter_(localDataCenter)
{ }

void TTrafficMeter::Start(TInstant now)
{
    TGuard<TAdaptiveLock> guard(Lock_);
    StartTime_ = now;
}

void TTrafficMeter::IncrementInboundByteCount(
    const TDataCenterName& sourceDataCenter,
    i64 byteCount)
{
    IncrementByteCount(sourceDataCenter, LocalDataCenter_, byteCount);

    IncrementByteCountImpl(InboundData_, sourceDataCenter, byteCount);
}

void TTrafficMeter::IncrementOutboundByteCount(
    const TDataCenterName& destinationDataCenter,
    i64 byteCount)
{
    IncrementByteCount(LocalDataCenter_, destinationDataCenter, byteCount);

    IncrementByteCountImpl(OutboundData_, destinationDataCenter, byteCount);
}

void TTrafficMeter::IncrementByteCount(
    const TDataCenterName& sourceDataCenter,
    const TDataCenterName& destinationDataCenter,
    i64 byteCount)
{
    TDirection direction(sourceDataCenter, destinationDataCenter);
    IncrementByteCountImpl(Data_, direction, byteCount);
}

THashMap<TDataCenterName, i64> TTrafficMeter::GetInboundByteCountBySource() const
{
    TGuard<TAdaptiveLock> guard(Lock_);
    return InboundData_;
}

THashMap<TDataCenterName, i64> TTrafficMeter::GetOutboundByteCountByDestination() const
{
    TGuard<TAdaptiveLock> guard(Lock_);
    return OutboundData_;
}

THashMap<TTrafficMeter::TDirection, i64> TTrafficMeter::GetByteCountByDirection() const

{
    TGuard<TAdaptiveLock> guard(Lock_);
    return Data_;
}

TDuration TTrafficMeter::GetDuration() const
{
    TGuard<TAdaptiveLock> guard(Lock_);

    YT_VERIFY(StartTime_ != TInstant::Zero());

    return TInstant::Now() - StartTime_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkClient
