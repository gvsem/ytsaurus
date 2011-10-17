﻿#include "table_reader.h"

#include "chunk_meta.pb.h"

#include "../misc/foreach.h"

#include <algorithm>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

struct TBlockInfo {
    int ChunkBlockIndex;
    int ChannelBlockIndex;
    int ChannelIndex;
    int LastRow;

    bool operator< (const TBlockInfo& rhs)
    {
        return (LastRow > rhs.LastRow) || 
            (LastRow == rhs.LastRow && ChannelIndex > rhs.ChannelIndex);
    }

    TBlockInfo(
        int chunkBlockIndex, 
        int channelBlockIndex, 
        int channelIdx, 
        int lastRow)
        : ChunkBlockIndex(chunkBlockIndex)
        , ChannelBlockIndex(channelBlockIndex)
        , ChannelIndex(channelIdx)
        , LastRow(lastRow)
    { }
};

////////////////////////////////////////////////////////////////////////////////

TTableReader::TTableReader(
    const TSequentialChunkReader::TConfig& config,
    const TChannel& channel,
    IChunkReader::TPtr chunkReader)
    : SequentialChunkReader(NULL)
    , Channel(channel)
    , InitSuccess(New< TFuture<bool> >())
    , IsColumnValid(false)
    , IsRowValid(false)
{
    VERIFY_THREAD_AFFINITY_ANY();

    // The last block contains meta.
    yvector<int> metaIndex(1, -1);
    chunkReader->AsyncReadBlocks(metaIndex)
        ->Subscribe(FromMethod(
            &TTableReader::OnGotMeta, 
            TPtr(this),
            config,
            chunkReader));
}

void TTableReader::OnGotMeta(
    IChunkReader::TReadResult readResult, 
    const TSequentialChunkReader::TConfig& config,
    IChunkReader::TPtr chunkReader)
{
    VERIFY_THREAD_AFFINITY_ANY();
    if (!readResult.IsOK) {
        InitSuccess->Set(false);
        return;
    }

    auto& metaBlob = readResult.Blocks.front();
    NProto::TChunkMeta protoMeta;
    protoMeta.ParseFromArray(metaBlob.Begin(), metaBlob.Size());

    yvector<TChannel> channels;
    channels.reserve(protoMeta.ChannelsSize());
    for(int i = 0; i < protoMeta.ChannelsSize(); ++i) {
        channels.push_back(TChannel::FromProto(protoMeta.GetChannels(i)));
    }

    yvector<int> selectedChannels;

    // Heuristic: first try to find a channel that contain the whole read channel.
    // If several exists, choose the one with minimum number of blocks.
    {
        int channelIdx = SelectSingleChannel(channels, protoMeta);
        if (channelIdx >= 0) {
            selectedChannels.push_back(channelIdx);
        } else {
            selectedChannels = SelectChannels(channels);
        }
    }

    yvector<int> blockIndexSequence = GetBlockReadingOrder(selectedChannels, protoMeta);
    SequentialChunkReader = New<TSequentialChunkReader>(config, blockIndexSequence, chunkReader);

    ChannelReaders.reserve(selectedChannels.size());
    FOREACH(int i, selectedChannels) {
        ChannelReaders.push_back(New<TChannelReader>(channels[i]));
    }

    InitSuccess->Set(true);
}

bool TTableReader::NextRow()
{
    VERIFY_THREAD_AFFINITY(Client);
    if (!InitSuccess->Get()) {
        ythrow yexception() << "Chunk reading failed.";
    }

    YASSERT(~SequentialChunkReader != NULL);

    CurrentChannel = 0;
    IsColumnValid = false;
    UsedColumns.clear();
    ++CurrentRow;

    YASSERT(CurrentRow <= RowCount);

    if (CurrentRow == RowCount) {
        IsRowValid = false;
        return false;
    }

    FOREACH(auto& channel, ChannelReaders) {
        if (!channel.NextRow()) {
            auto result = SequentialChunkReader->AsyncGetNextBlock()->Get();
            if (!result.IsOK) {
                ythrow yexception() << "Chunk reading failed.";
            }
            channel.SetBlock(result.Block);
            YVERIFY(channel.NextRow());
        }
    }

    IsRowValid = true;
    return true;
}

bool TTableReader::NextColumn()
{
    VERIFY_THREAD_AFFINITY(Client);
    YASSERT(InitSuccess->IsSet());
    YASSERT(IsRowValid);

    while (true) {
        if (CurrentChannel >= ChannelReaders.ysize()) {
            IsColumnValid = false;
            return false;
        }

        auto& channelReader = ChannelReaders[CurrentChannel];
        if (channelReader.NextColumn()) {
            CurrentColumn = channelReader.GetColumn();
            if (!Channel.Contains(CurrentColumn)) {
                continue;
            }

            if (!UsedColumns.has(CurrentColumn)) {
                UsedColumns.insert(CurrentColumn);
                IsColumnValid = true;
                return true;
            }
        } else {
            ++CurrentChannel;
        }
    }

    YUNREACHABLE();
}

const TColumn& TTableReader::GetColumn() const
{
    VERIFY_THREAD_AFFINITY(Client);
    YASSERT(InitSuccess->IsSet());

    YASSERT(IsRowValid);
    YASSERT(IsColumnValid);

    return CurrentColumn;
}

TValue TTableReader::GetValue() const
{
    VERIFY_THREAD_AFFINITY(Client);
    YASSERT(InitSuccess->IsSet());

    YASSERT(IsRowValid);
    YASSERT(IsColumnValid);

    return ChannelReaders[CurrentChannel]->GetValue();
}

yvector<int> TTableReader::SelectChannels(const yvector<TChannel>& channels)
{
    yvector<int> result;

    TChannel remainder = Channel;
    for (int channelIdx = 0; channelIdx < channels.size(); ++channelIdx) {
        auto& curChannel = channels[channelIdx];
        if (curChannel.Overlaps(remainder)) {
            remainder -= curChannel;
            result.push_back(channelIdx);
            if (remainder.IsEmpty()) {
                break;
            }
        }
    }

    return result;
}

int TTableReader::SelectSingleChannel(
    const yvector<TChannel>& channels, 
    const NProto::TChunkMeta& protoMeta)
{
    int resultIdx = -1;
    int minBlockCount = -1;

    for (int channelIdx = 0; channelIdx < channels.size(); ++channelIdx) {
        if (channels[channelIdx].Contains(Channel)) {
            int blockCount = protoMeta.GetChannels(channelIdx).BlocksSize();
            if (resultIdx < 0 || minBlockCount > blockCount) {
                resultIdx = channelIdx;
                minBlockCount = blockCount;
            }
        }
    }
    return resultIdx;
}

// Note: sets RowCount as side effect
yvector<int> TTableReader::GetBlockReadingOrder(
    const yvector<int>& selectedChannels, 
    const NProto::TChunkMeta& protoMeta)
{
    yvector<int> result;

    yvector<TBlockInfo> blockHeap;
    FOREACH (auto channelIdx, selectedChannels) {
        const auto& protoBlock =  protoMeta.GetChannels(channelIdx).GetBlocks(0);
        blockHeap.push_back(TBlockInfo(
            protoBlock.GetBlockIndex(),
            0,
            channelIdx,
            protoBlock.GetRowCount()));

        result.push_back(protoBlock.GetBlockIndex());
    }
    std::make_heap(blockHeap.begin(), blockHeap.end());

    while (!blockHeap.empty()) {
        TBlockInfo currentBlock = blockHeap.front();
        int nextBlockIndex = currentBlock.ChannelBlockIndex + 1;
        const auto& protoChannel = protoMeta.GetChannels(currentBlock.ChannelIndex);

        std::pop_heap(blockHeap.begin(), blockHeap.end());
        blockHeap.pop_back();

        if (nextBlockIndex < protoChannel.BlocksSize()) {
            const auto& protoBlock = protoChannel.GetBlocks(nextBlockIndex);

            blockHeap.push_back(TBlockInfo(
                protoBlock.GetBlockIndex(),
                nextBlockIndex,
                currentBlock.ChannelIndex,
                currentBlock.LastRow + protoBlock.GetRowCount()));

            std::push_heap(blockHeap.begin(), blockHeap.end());
            result.push_back(protoBlock.GetBlockIndex());
        } else {
            if (RowCount == 0) {
                // RowCount is not set yet
                RowCount = currentBlock.LastRow;
            } else {
                YASSERT(RowCount == currentBlock.LastRow);
            }
        }
    }

    return result;
}


////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
