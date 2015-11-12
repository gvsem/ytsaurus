#pragma once

#include <yt/core/misc/public.h>

namespace NYT {
namespace NBus {

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_STRUCT(IBus)
DECLARE_REFCOUNTED_STRUCT(IMessageHandler)
DECLARE_REFCOUNTED_STRUCT(IBusClient)
DECLARE_REFCOUNTED_STRUCT(IBusServer)

struct TTcpDispatcherStatistics;
struct TTcpProfilingData;

DECLARE_REFCOUNTED_CLASS(TTcpBusConfig)
DECLARE_REFCOUNTED_CLASS(TTcpBusServerConfig)
DECLARE_REFCOUNTED_CLASS(TTcpBusClientConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NBus
} // namespace NYT

