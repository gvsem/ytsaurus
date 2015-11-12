#pragma once

#include "public.h"

#include <yt/ytlib/chunk_client/config.h>

#include <yt/core/ytree/yson_serializable.h>

namespace NYT {
namespace NFileClient {

////////////////////////////////////////////////////////////////////////////////

class TFileChunkWriterConfig
    : public virtual NChunkClient::TEncodingWriterConfig
{
public:
    i64 BlockSize;

    TFileChunkWriterConfig()
    {
        RegisterParameter("block_size", BlockSize)
            .Default((i64) 16 * 1024 * 1024)
            .GreaterThan(0);
    }
};

DEFINE_REFCOUNTED_TYPE(TFileChunkWriterConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileClient
} // namespace NYT
