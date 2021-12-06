#pragma once

#include "public.h"

#include <yt/yt/core/rpc/service_detail.h>

namespace NYT::NHydra2 {

////////////////////////////////////////////////////////////////////////////////

class THydraServiceBase
    : public NRpc::TServiceBase
{
protected:
    THydraServiceBase(
        IInvokerPtr invoker,
        const NRpc::TServiceDescriptor& descriptor,
        const NLogging::TLogger& logger,
        NRpc::TRealmId realmId);

    void ValidatePeer(EPeerKind kind);
    void SyncWithUpstream();

    virtual IHydraManagerPtr GetHydraManager() = 0;
    virtual TFuture<void> DoSyncWithUpstream();

private:
    bool IsUp(const TCtxDiscoverPtr& context) override;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra2
