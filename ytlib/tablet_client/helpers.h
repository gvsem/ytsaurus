#pragma once

#include "public.h"

#include <yt/core/ypath/public.h>

namespace NYT::NTabletClient {

////////////////////////////////////////////////////////////////////////////////

NYPath::TYPath GetCypressClustersPath();
NYPath::TYPath GetCypressClusterPath(const TString& name);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletClient

