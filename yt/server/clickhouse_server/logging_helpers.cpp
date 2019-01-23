#include "logging_helpers.h"

#include "type_helpers.h"

#include <util/generic/yexception.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

std::string CurrentExceptionText()
{
    return ToStdString(::CurrentExceptionMessage());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
