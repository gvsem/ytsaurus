#include "stdafx.h"
#include "chunk_spec.h"
#include "chunk_meta_extensions.h"
#include "chunk_replica.h"

#include <core/misc/protobuf_helpers.h>

#include <core/erasure/codec.h>

#include <core/ytree/attribute_helpers.h>

namespace NYT {
namespace NChunkClient {

using namespace NChunkClient::NProto;

////////////////////////////////////////////////////////////////////////////////

TRefCountedChunkSpec::TRefCountedChunkSpec()
{ }

TRefCountedChunkSpec::TRefCountedChunkSpec(const TRefCountedChunkSpec& other)
{
    CopyFrom(other);
}

TRefCountedChunkSpec::TRefCountedChunkSpec(TRefCountedChunkSpec&& other)
{
    Swap(&other);
}

TRefCountedChunkSpec::TRefCountedChunkSpec(const TChunkSpec& other)
{
    CopyFrom(other);
}

TRefCountedChunkSpec::TRefCountedChunkSpec(TChunkSpec&& other)
{
    Swap(&other);
}

////////////////////////////////////////////////////////////////////////////////

bool IsUnavailable(const TChunkReplicaList& replicas, NErasure::ECodec codecId)
{
    if (codecId == NErasure::ECodec::None) {
        return replicas.empty();
    } else {
        auto* codec = NErasure::GetCodec(codecId);
        int dataPartCount = codec->GetDataPartCount();
        NErasure::TPartIndexSet missingIndexSet((1 << dataPartCount) - 1);
        for (auto replica : replicas) {
            missingIndexSet.reset(replica.GetIndex());
        }
        return missingIndexSet.any();
    }
}

bool IsUnavailable(const NProto::TChunkSpec& chunkSpec)
{
    auto codecId = NErasure::ECodec(chunkSpec.erasure_codec());
    auto replicas = NYT::FromProto<TChunkReplica, TChunkReplicaList>(chunkSpec.replicas());
    return IsUnavailable(replicas, codecId);
}

void GetStatistics(
    const TChunkSpec& chunkSpec,
    i64* dataSize,
    i64* rowCount,
    i64* valueCount)
{
    auto miscExt = GetProtoExtension<TMiscExt>(chunkSpec.chunk_meta().extensions());
    auto sizeOverrideExt = FindProtoExtension<TSizeOverrideExt>(chunkSpec.chunk_meta().extensions());

    if (sizeOverrideExt) {
        if (dataSize) {
            *dataSize = sizeOverrideExt->uncompressed_data_size();
        }
        if (rowCount) {
            *rowCount = sizeOverrideExt->row_count();
        }
    } else {
        if (dataSize) {
            *dataSize = miscExt.uncompressed_data_size();
        }
        if (rowCount) {
            *rowCount = miscExt.row_count();
        }
    }

    if (valueCount) {
        *valueCount = miscExt.value_count();
    }
}

TRefCountedChunkSpecPtr CreateCompleteChunk(TRefCountedChunkSpecPtr chunkSpec)
{
    auto result = New<TRefCountedChunkSpec>(*chunkSpec);
    result->clear_start_limit();
    result->clear_end_limit();

    RemoveProtoExtension<TSizeOverrideExt>(result->mutable_chunk_meta()->mutable_extensions());

    return result;
}

TChunkId EncodeChunkId(
    const TChunkSpec& chunkSpec,
    NNodeTrackerClient::TNodeId nodeId)
{
    auto replicas = NYT::FromProto<TChunkReplica, TChunkReplicaList>(chunkSpec.replicas());
    auto replicaIt = std::find_if(
        replicas.begin(),
        replicas.end(),
        [=] (TChunkReplica replica) {
            return replica.GetNodeId() == nodeId;
        });
    YCHECK(replicaIt != replicas.end());

    TChunkIdWithIndex chunkIdWithIndex(
        NYT::FromProto<TChunkId>(chunkSpec.chunk_id()),
        replicaIt->GetIndex());
    return EncodeChunkId(chunkIdWithIndex);
}

bool ExtractOverwriteFlag(const NYTree::IAttributeDictionary& attributes)
{
    return !attributes.Get<bool>("append", false);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
