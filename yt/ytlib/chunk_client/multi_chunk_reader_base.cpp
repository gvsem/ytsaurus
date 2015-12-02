#include "stdafx.h"

#include "multi_chunk_reader_base.h"

#include "block_cache.h"
#include "chunk_meta_extensions.h"
#include "chunk_reader_base.h"
#include "chunk_spec.h"
#include "config.h"
#include "dispatcher.h"
#include "helpers.h"
#include "private.h"

#include <ytlib/api/client.h>
#include <ytlib/api/connection.h>
#include <ytlib/api/config.h>

#include <ytlib/node_tracker_client/node_directory.h>

#include <core/concurrency/scheduler.h>

#include <core/erasure/codec.h>

#include <core/misc/protobuf_helpers.h>

namespace NYT {
namespace NChunkClient {

using namespace NConcurrency;
using namespace NErasure;
using namespace NProto;
using namespace NNodeTrackerClient;
using namespace NApi;

////////////////////////////////////////////////////////////////////////////////

static i64 GetMemoryEstimate(const TChunkSpec& chunkSpec, TMultiChunkReaderConfigPtr config)
{
    i64 currentSize;
    GetStatistics(chunkSpec, &currentSize);
    auto miscExt = GetProtoExtension<TMiscExt>(chunkSpec.chunk_meta().extensions());

    // Block used by upper level chunk reader.
    i64 chunkBufferSize = ChunkReaderMemorySize + miscExt.max_block_size();

    if (currentSize > miscExt.max_block_size()) {
        chunkBufferSize += config->WindowSize + config->GroupSize;
    }

    return chunkBufferSize;
}

static TMultiChunkReaderConfigPtr PatchConfig(TMultiChunkReaderConfigPtr config, i64 memoryEstimate)
{
    if (memoryEstimate > config->WindowSize + config->GroupSize) {
        return config;
    }

    auto newConfig = CloneYsonSerializable(config);
    newConfig->WindowSize = std::max(memoryEstimate / 2, (i64) 1);
    newConfig->GroupSize = std::max(memoryEstimate / 2, (i64) 1);
    return newConfig;
}

////////////////////////////////////////////////////////////////////////////////

TMultiChunkReaderBase::TMultiChunkReaderBase(
    TMultiChunkReaderConfigPtr config,
    TMultiChunkReaderOptionsPtr options,
    IClientPtr client,
    IBlockCachePtr blockCache,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const std::vector<NProto::TChunkSpec>& chunkSpecs,
    IThroughputThrottlerPtr throttler)
    : Logger(ChunkClientLogger)
    , Config_(config)
    , Options_(options)
    , Throttler_(throttler)
    , CompletionError_(NewPromise<void>())
    , BlockCache_(blockCache)
    , Client_(client)
    , NodeDirectory_(nodeDirectory)
    , FreeBufferSize_(Config_->MaxBufferSize)
{
    Logger.AddTag("Reader: %v", this);

    CurrentSession_.Reset();

    LOG_DEBUG("Creating multi chunk reader for %v chunks",
        chunkSpecs.size());

    if (chunkSpecs.empty()) {
        CompletionError_.Set(TError());
        return;
    }

    for (const auto& chunkSpec : chunkSpecs) {
        Chunks_.emplace_back(TChunk{chunkSpec, GetMemoryEstimate(chunkSpec, Config_)});
    }
}

TFuture<void> TMultiChunkReaderBase::Open()
{
    YCHECK(!IsOpen_);
    IsOpen_ = true;
    if (CompletionError_.IsSet()) {
        ReadyEvent_ = CompletionError_.ToFuture();
    } else {
        ReadyEvent_ = BIND(&TMultiChunkReaderBase::DoOpen, MakeStrong(this))
            .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
            .Run();
    }

    return ReadyEvent_;
}

TFuture<void> TMultiChunkReaderBase::GetReadyEvent()
{
    return ReadyEvent_;
}

TDataStatistics TMultiChunkReaderBase::GetDataStatistics() const
{
    TGuard<TSpinLock> guard(DataStatisticsLock_);
    auto dataStatistics = DataStatistics_;
    for (auto reader : ActiveReaders_) {
        dataStatistics += reader->GetDataStatistics();
    }
    return dataStatistics;
}

bool TMultiChunkReaderBase::IsFetchingCompleted() const
{
    return FetchingCompleted_.IsSet();
}

std::vector<TChunkId> TMultiChunkReaderBase::GetFailedChunkIds() const
{
    TGuard<TSpinLock> guard(FailedChunksLock_);
    return FailedChunks_;
}

void TMultiChunkReaderBase::OpenNextChunks()
{
    TGuard<TSpinLock> guard(PrefetchLock_);
    for (; PrefetchIndex_ < Chunks_.size(); ++PrefetchIndex_) {
        if (Chunks_[PrefetchIndex_].MemoryEstimate > FreeBufferSize_ &&
            ActiveReaderCount_ > 0 &&
            !Options_->KeepInMemory) 
        {
            return;
        }

        if (ActiveReaderCount_ > Config_->MaxPrefetchWindow) {
            return;
        }

        ++ActiveReaderCount_;
        FreeBufferSize_ -= Chunks_[PrefetchIndex_].MemoryEstimate;
        BIND(
            &TMultiChunkReaderBase::DoOpenChunk,
            MakeWeak(this),
            PrefetchIndex_)
        .Via(TDispatcher::Get()->GetReaderInvoker())
        .Run();
    }
}

void TMultiChunkReaderBase::DoOpenChunk(int chunkIndex)
{
    if (CompletionError_.IsSet())
        return;

    const auto& chunk = Chunks_[chunkIndex];

    LOG_DEBUG("Opening chunk (ChunkIndex: %v)", chunkIndex);
    auto remoteReader = CreateRemoteReader(chunk);

    auto reader = CreateTemplateReader(chunk.Spec, remoteReader);
    auto error = WaitFor(reader->Open());

    if (!error.IsOK()) {
        RegisterFailedChunk(chunkIndex);
        CompletionError_.TrySet(error);
        return;
    }

    if (CompletionError_.IsSet())
        return;

    OnReaderOpened(reader, chunkIndex);

    FetchingCompletedEvents_.push_back(reader->GetFetchingCompletedEvent());
    if (++OpenedReaderCount_ == Chunks_.size()) {
        FetchingCompleted_.SetFrom(Combine(FetchingCompletedEvents_));
    }

    TGuard<TSpinLock> guard(DataStatisticsLock_);
    YCHECK(ActiveReaders_.insert(reader).second);
}

IChunkReaderPtr TMultiChunkReaderBase::CreateRemoteReader(const TChunk& chunk)
{
    const auto& chunkSpec = chunk.Spec;
    auto config = PatchConfig(Config_, chunk.MemoryEstimate);

    auto chunkId = NYT::FromProto<TChunkId>(chunkSpec.chunk_id());
    auto replicas = NYT::FromProto<TChunkReplica, TChunkReplicaList>(chunkSpec.replicas());
    auto erasureCodecId = ECodec(chunkSpec.erasure_codec());

    return NChunkClient::CreateRemoteReader(
        chunkId,
        std::move(replicas),
        erasureCodecId,
        config, 
        Options_, 
        Client_, 
        NodeDirectory_, 
        BlockCache_, 
        Throttler_);
}

void TMultiChunkReaderBase::OnReaderFinished()
{
    if (Options_->KeepInMemory) {
        FinishedReaders_.push_back(CurrentSession_.ChunkReader);
    }

    {
        TGuard<TSpinLock> guard(DataStatisticsLock_);
        DataStatistics_ += CurrentSession_.ChunkReader->GetDataStatistics();
        YCHECK(ActiveReaders_.erase(CurrentSession_.ChunkReader));
    }

    --ActiveReaderCount_;
    FreeBufferSize_ += Chunks_[CurrentSession_.ChunkIndex].MemoryEstimate;

    CurrentSession_.Reset();
    OpenNextChunks();
}

bool TMultiChunkReaderBase::OnEmptyRead(bool readerFinished)
{
    if (readerFinished) {
        OnReaderFinished();
        return !CompletionError_.IsSet() || !CompletionError_.Get().IsOK();
    } else {
        OnReaderBlocked();
        return true;
    }
}

void TMultiChunkReaderBase::OnError()
{ }

TFuture<void> TMultiChunkReaderBase::CombineCompletionError(TFuture<void> future)
{
    auto promise = NewPromise<void>();
    promise.TrySetFrom(CompletionError_.ToFuture());
    promise.TrySetFrom(future);
    return promise.ToFuture();
}

void TMultiChunkReaderBase::RegisterFailedChunk(int chunkIndex)
{   
    auto chunkId = NYT::FromProto<TChunkId>(Chunks_[chunkIndex].Spec.chunk_id());
    LOG_WARNING("Chunk reader failed (ChunkId: %v)", chunkId);

    OnError();

    TGuard<TSpinLock> guard(FailedChunksLock_);
    FailedChunks_.push_back(chunkId);
}

////////////////////////////////////////////////////////////////////////////////

TSequentialMultiChunkReaderBase::TSequentialMultiChunkReaderBase(
    TMultiChunkReaderConfigPtr config,
    TMultiChunkReaderOptionsPtr options,
    IClientPtr client,
    NChunkClient::IBlockCachePtr blockCache,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const std::vector<NProto::TChunkSpec>& chunkSpecs,
    IThroughputThrottlerPtr throttler)
    : TMultiChunkReaderBase(
        config, 
        options, 
        client, 
        blockCache, 
        nodeDirectory,
        chunkSpecs,
        throttler)
{
    NextReaders_.reserve(Chunks_.size());
    for (int i = 0; i < Chunks_.size(); ++i) {
        NextReaders_.push_back(NewPromise<IChunkReaderBasePtr>());
    }
}

void TSequentialMultiChunkReaderBase::DoOpen()
{
    OpenNextChunks();
    WaitForNextReader();
}

void TSequentialMultiChunkReaderBase::OnReaderOpened(IChunkReaderBasePtr chunkReader, int chunkIndex)
{
    // May have already been set in case of error.
    NextReaders_[chunkIndex].TrySet(chunkReader);
}

void TSequentialMultiChunkReaderBase::OnReaderBlocked()
{
    ReadyEvent_ = CombineCompletionError(BIND(
        &TSequentialMultiChunkReaderBase::WaitForCurrentReader,
        MakeStrong(this))
    .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
    .Run());
}

void TSequentialMultiChunkReaderBase::OnReaderFinished()
{
    TMultiChunkReaderBase::OnReaderFinished();

    ++FinishedReaderCount_;
    if (FinishedReaderCount_ == Chunks_.size()) {
        CompletionError_.TrySet(TError());
        return;
    }

    ReadyEvent_ = CombineCompletionError(BIND(
        &TSequentialMultiChunkReaderBase::WaitForNextReader,
        MakeStrong(this))
    .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
    .Run());
}

void TSequentialMultiChunkReaderBase::WaitForNextReader()
{
    if (NextReaderIndex_ == Chunks_.size()) {
        return;
    }

    CurrentSession_.ChunkIndex = NextReaderIndex_;
    CurrentSession_.ChunkReader = WaitFor(NextReaders_[NextReaderIndex_].ToFuture())
        .ValueOrThrow();

    ++NextReaderIndex_;

    // Avoid memory leaks, drop smart pointer reference.
    NextReaders_[CurrentSession_.ChunkIndex].Reset();

    OnReaderSwitched();
}

void TSequentialMultiChunkReaderBase::WaitForCurrentReader()
{
    auto error = WaitFor(CurrentSession_.ChunkReader->GetReadyEvent());
    if (!error.IsOK()) {
        RegisterFailedChunk(CurrentSession_.ChunkIndex);
        CompletionError_.TrySet(error);
    }
}

void TSequentialMultiChunkReaderBase::OnError()
{
    BIND([=, this_ = MakeStrong(this)] () {
        // This is to avoid infinite waiting and memory leaks.
        for (int i = NextReaderIndex_; i < NextReaders_.size(); ++i) {
            NextReaders_[i].Reset();
        }
        NextReaderIndex_ = NextReaders_.size();
    })
    .Via(TDispatcher::Get()->GetReaderInvoker())
    .Run();
}

////////////////////////////////////////////////////////////////////////////////

TParallelMultiChunkReaderBase::TParallelMultiChunkReaderBase(
    TMultiChunkReaderConfigPtr config,
    TMultiChunkReaderOptionsPtr options,
    IClientPtr client,
    IBlockCachePtr blockCache,
    NNodeTrackerClient::TNodeDirectoryPtr nodeDirectory,
    const std::vector<NProto::TChunkSpec>& chunkSpecs,
    IThroughputThrottlerPtr throttler)
    : TMultiChunkReaderBase(
        config,
        options,
        client,
        blockCache,
        nodeDirectory,
        chunkSpecs,
        throttler)
{ }

void TParallelMultiChunkReaderBase::DoOpen()
{
    OpenNextChunks();
    WaitForReadyReader();
}

void TParallelMultiChunkReaderBase::OnReaderOpened(IChunkReaderBasePtr chunkReader, int chunkIndex)
{
    TSession session;
    session.ChunkReader = chunkReader;
    session.ChunkIndex = chunkIndex;

    ReadySessions_.Enqueue(session);
}

void TParallelMultiChunkReaderBase::OnReaderBlocked()
{
    BIND(
        &TParallelMultiChunkReaderBase::WaitForReader,
        MakeStrong(this),
        CurrentSession_)
    .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
    .Run();

    CurrentSession_.Reset();

    ReadyEvent_ = CombineCompletionError(BIND(
        &TParallelMultiChunkReaderBase::WaitForReadyReader,
        MakeStrong(this))
    .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
    .Run());
}

void TParallelMultiChunkReaderBase::OnReaderFinished()
{
    TMultiChunkReaderBase::OnReaderFinished();

    ++FinishedReaderCount_;
    if (FinishedReaderCount_ == Chunks_.size()) {
        ReadySessions_.Enqueue(TError("Sentinel session"));
        CompletionError_.TrySet(TError());
    } else {
        ReadyEvent_ = CombineCompletionError(BIND(
            &TParallelMultiChunkReaderBase::WaitForReadyReader,
            MakeStrong(this))
        .AsyncVia(TDispatcher::Get()->GetReaderInvoker())
        .Run());
    }
}

void TParallelMultiChunkReaderBase::OnError()
{
    // Someone may wait for this future.
    ReadySessions_.Enqueue(TError("Sentinel session"));
}

void TParallelMultiChunkReaderBase::WaitForReadyReader()
{
    auto asyncReadySession = ReadySessions_.Dequeue();
    CurrentSession_ = WaitFor(asyncReadySession)
        .ValueOrThrow();

    OnReaderSwitched();
}

void TParallelMultiChunkReaderBase::WaitForReader(TSession session)
{
    auto error = WaitFor(session.ChunkReader->GetReadyEvent());
    if (error.IsOK()) {
        ReadySessions_.Enqueue(session);
        return;
    }

    RegisterFailedChunk(session.ChunkIndex);
    CompletionError_.TrySet(error);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
