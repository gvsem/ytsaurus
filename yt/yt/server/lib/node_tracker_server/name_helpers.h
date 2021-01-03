#pragma once

#include <util/generic/string.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

void ValidateDataCenterName(const TString& name);

void ValidateRackName(const TString& name);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
