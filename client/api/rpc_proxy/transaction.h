#pragma once

#include "private.h"

#include <yt/core/rpc/public.h>

#include <yt/client/api/public.h>

namespace NYT {
namespace NApi {
namespace NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

NApi::ITransactionPtr CreateTransaction(
    TConnectionPtr connection,
    TClientPtr client,
    NRpc::IChannelPtr channel,
    const NTransactionClient::TTransactionId& id,
    NTransactionClient::TTimestamp startTimestamp,
    NTransactionClient::ETransactionType type,
    NTransactionClient::EAtomicity atomicity,
    NTransactionClient::EDurability durability,
    TDuration timeout,
    TNullable<TDuration> pingPeriod,
    bool sticky);

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpcProxy
} // namespace NApi
} // namespace NYT
