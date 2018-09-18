#pragma once

#include <yt/server/clickhouse_server/interop/api.h>

#include <Poco/AutoPtr.h>
#include <Poco/Logger.h>

namespace NYT {
namespace NClickHouse {

////////////////////////////////////////////////////////////////////////////////

Poco::AutoPtr<Poco::Channel> WrapToLogChannel(NInterop::ILoggerPtr logger);

}   // namespace NClickHouse
}   // namespace NYT
