#pragma once

#include "public.h"

namespace NYT::NScheduler {

////////////////////////////////////////////////////////////////////////////////

template <class TAnyFairShareTree>
ISchedulerStrategyPtr CreateFairShareStrategy(
    TFairShareStrategyConfigPtr config,
    ISchedulerStrategyHost* host,
    std::vector<IInvokerPtr> feasibleInvokers);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NScheduler
