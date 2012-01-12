#pragma once

#include "error.h"

#include <ytlib/misc/guid.h>

#include <ytlib/logging/log.h>

namespace NYT {
namespace NRpc {

////////////////////////////////////////////////////////////////////////////////

extern NLog::TLogger RpcLogger;

////////////////////////////////////////////////////////////////////////////////

typedef TGuid TRequestId;
extern TRequestId NullRequestId;

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpc
} // namespace NYT
