#pragma once

#include "../rpc/client.h"
#include "../misc/lazy_ptr.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

extern TLazyHolder<NRpc::TChannelCache> HolderChannelCache;

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT


