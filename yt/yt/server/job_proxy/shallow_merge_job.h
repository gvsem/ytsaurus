#pragma once

#include "public.h"

namespace NYT::NJobProxy {

////////////////////////////////////////////////////////////////////////////////

IJobPtr CreateShallowMergeJob(IJobHost* host);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobProxy
