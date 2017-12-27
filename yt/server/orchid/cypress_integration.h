#pragma once

#include "public.h"

#include <yt/server/cell_master/public.h>

#include <yt/server/cypress_server/public.h>

#include <yt/ytlib/node_tracker_client/public.h>

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/rpc/config.h>

namespace NYT {
namespace NOrchid {

////////////////////////////////////////////////////////////////////////////////

struct TOrchidManifest
    : public NYTree::TYsonSerializable
{
    NNodeTrackerClient::TAddressMap RemoteAddresses;
    TString RemoteRoot;
    TDuration Timeout;
    NRpc::TRetryingChannelConfigPtr RetriesConfig;

    TOrchidManifest()
    {
        RegisterParameter("remote_addresses", RemoteAddresses);
        RegisterParameter("remote_root", RemoteRoot)
            .Default("/");
        RegisterParameter("timeout", Timeout)
            .Default(TDuration::Seconds(15));
        RegisterParameter("retries_config", RetriesConfig)
            .DefaultNew();
    }
};

DEFINE_REFCOUNTED_TYPE(TOrchidManifest)

////////////////////////////////////////////////////////////////////////////////

NCypressServer::INodeTypeHandlerPtr CreateOrchidTypeHandler(NCellMaster::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NOrchid
} // namespace NYT
