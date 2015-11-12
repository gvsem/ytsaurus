#pragma once

#include "public.h"

#include <yt/core/actions/future.h>

#include <yt/core/misc/ref_counted.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

struct IReaderBase
    : public virtual TRefCounted
{
    virtual TFuture<void> Open() = 0;

    virtual TFuture<void> GetReadyEvent() = 0;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
