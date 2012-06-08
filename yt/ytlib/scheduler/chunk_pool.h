#pragma once

#include "private.h"
#include "progress_counter.h"

#include <ytlib/misc/small_vector.h>
#include <ytlib/chunk_server/public.h>
#include <ytlib/table_client/table_reader.pb.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

struct TWeightedChunk
{
    NTableClient::NProto::TInputChunk InputChunk;
    i64 Weight;
};

////////////////////////////////////////////////////////////////////////////////

struct TChunkStripe
    : public TIntrinsicRefCounted
{
    TChunkStripe();
    TChunkStripe(const NTableClient::NProto::TInputChunk& inputChunk, i64 weight);

    void AddChunk(const NTableClient::NProto::TInputChunk& inputChunk, i64 weight);

    std::vector<NChunkServer::TChunkId> GetChunkIds() const;

    TSmallVector<TWeightedChunk, 1> Chunks;
    i64 Weight;
};

////////////////////////////////////////////////////////////////////////////////

struct TPoolExtractionResult
    : public TIntrinsicRefCounted
{
    TPoolExtractionResult();

    void Add(TChunkStripePtr stripe, const Stroka& address);

    std::vector<TChunkStripePtr> Stripes;
    i64 TotalChunkWeight;
    int TotalChunkCount;
    int LocalChunkCount;
    int RemoteChunkCount;

};

////////////////////////////////////////////////////////////////////////////////

struct IChunkPool
{
    virtual ~IChunkPool()
    { }

    virtual void Add(TChunkStripePtr stripe) = 0;

    virtual TPoolExtractionResultPtr Extract(
        const Stroka& address,
        TNullable<i64> weightThreshold) = 0;
    virtual void OnFailed(TPoolExtractionResultPtr result) = 0;
    virtual void OnCompleted(TPoolExtractionResultPtr result) = 0;

    virtual const TProgressCounter& WeightCounter() const = 0;
    virtual const TProgressCounter& ChunkCounter() const = 0;

    virtual bool IsCompleted() const = 0;
    virtual bool IsPending() const = 0;
    
    virtual i64 GetLocality(const Stroka& address) const = 0;
};

////////////////////////////////////////////////////////////////////

TAutoPtr<IChunkPool> CreateUnorderedChunkPool();
TAutoPtr<IChunkPool> CreateAtomicChunkPool();

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
