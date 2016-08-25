#pragma once

#include "input_chunk_slice.h"
#include "public.h"

#include <yt/core/misc/nullable.h>
#include <yt/core/misc/phoenix.h>

#include <yt/ytlib/table_client/data_slice_descriptor.h>
#include <yt/ytlib/table_client/schema.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

struct TInputDataSlice
    : public TIntrinsicRefCounted
{
    using TChunkSliceList = SmallVector<TInputChunkSlicePtr, 1>;

    DEFINE_BYREF_RO_PROPERTY(TInputSliceLimit, LowerLimit);
    DEFINE_BYREF_RO_PROPERTY(TInputSliceLimit, UpperLimit);

public:
    TInputDataSlice() = default;
    TInputDataSlice(
        NTableClient::EDataSliceDescriptorType type,
        TChunkSliceList chunkSlices,
        TInputSliceLimit lowerLimit,
        TInputSliceLimit upperLimit);

    int GetChunkCount() const;
    i64 GetDataSize() const;
    i64 GetRowCount() const;
    i64 GetMaxBlockSize() const;

    int GetTableIndex() const;

    void Persist(NTableClient::TPersistenceContext& context);

    // Check that data slice is an old single-chunk slice. Used for compatibility.
    bool IsTrivial() const;

    TInputChunkPtr GetSingleUnversionedChunkOrThrow() const;

    TChunkSliceList ChunkSlices;
    NTableClient::EDataSliceDescriptorType Type;
};

DEFINE_REFCOUNTED_TYPE(TInputDataSlice)

////////////////////////////////////////////////////////////////////////////////

void ToProto(
    NTableClient::NProto::TDataSliceDescriptor* dataSliceDescriptor,
    TInputDataSlicePtr inputDataSlice,
    const NTableClient::TTableSchema& schema,
    NTableClient::TTimestamp timestamp);

////////////////////////////////////////////////////////////////////////////////

TInputDataSlicePtr CreateInputDataSlice(
    NTableClient::EDataSliceDescriptorType type,
    const std::vector<TInputChunkPtr>& inputChunks,
    NTableClient::TKey lower,
    NTableClient::TKey upper);

TInputDataSlicePtr CreateInputDataSlice(
    const TInputDataSlicePtr& dataSlice,
    NTableClient::TKey lowerKey = NTableClient::TKey(),
    NTableClient::TKey upperKey = NTableClient::TKey());

TInputDataSlicePtr CreateInputDataSlice(TInputChunkSlicePtr chunkSlice);

TNullable<TChunkId> IsUnavailable(const TInputDataSlicePtr& dataSlice, bool checkParityParts);
bool CompareDataSlicesByLowerLimit(const TInputDataSlicePtr& slice1, const TInputDataSlicePtr& slice2);
bool CanMergeSlices(const TInputDataSlicePtr& slice1, const TInputDataSlicePtr& slice2);

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
