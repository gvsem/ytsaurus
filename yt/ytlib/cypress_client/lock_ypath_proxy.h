#pragma once

#include "public.h"

#include <ytlib/rpc/public.h>
#include <ytlib/rpc/rpc.pb.h>

#include <ytlib/object_client/object_ypath_proxy.h>

#include <ytlib/cypress_client/lock_ypath.pb.h>

namespace NYT {
namespace NCypressClient {

////////////////////////////////////////////////////////////////////////////////

struct TLockYPathProxy
    : public NObjectClient::TObjectYPathProxy
{ };

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressClient
} // namespace NYT
