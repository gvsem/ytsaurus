#pragma once

#include "public.h"

#include <yt/core/bus/config.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NTracing {

////////////////////////////////////////////////////////////////////////////////

class TTraceManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Address where all trace events are pushed to.
    //! If |Null| then push is disabled.
    TNullable<TString> Address;

    //! Maximum number of trace events per batch.
    int MaxBatchSize;

    //! Send period.
    TDuration SendPeriod;

    //! Port to show in endpoints.
    ui16 EndpointPort;

    // Bus config.
    NBus::TTcpBusConfigPtr BusClient;

    TTraceManagerConfig()
    {
        RegisterParameter("address", Address)
            .Default();
        RegisterParameter("max_batch_size", MaxBatchSize)
            .Default(100);
        RegisterParameter("send_period", SendPeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("endpoint_port", EndpointPort)
            .Default(0);
        RegisterParameter("bus_client", BusClient)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TTraceManagerConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NTracing
} // namespace NYT

