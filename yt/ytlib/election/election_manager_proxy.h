#pragma once

#include "common.h"
#include <ytlib/election/election_manager.pb.h>

#include <ytlib/rpc/client.h>

namespace NYT {
namespace NElection {

////////////////////////////////////////////////////////////////////////////////

class TElectionManagerProxy
    : public NRpc::TProxyBase
{
public:
    typedef TIntrusivePtr<TElectionManagerProxy> TPtr;

    static Stroka GetServiceName()
    {
        return "ElectionManager";
    }

    TElectionManagerProxy(NRpc::IChannelPtr channel)
        : TProxyBase(channel, GetServiceName())
    { }

    DEFINE_RPC_PROXY_METHOD(NElection::NProto, PingFollower);
    DEFINE_RPC_PROXY_METHOD(NElection::NProto, GetStatus);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT
