#pragma once

#include "async_reader.h"
#include "format.h"
#include <ytlib/chunk_holder/chunk.pb.h>

#include <util/system/file.h>

namespace NYT {
namespace NChunkClient {

///////////////////////////////////////////////////////////////////////////////

//! Provides a local and synchronous implementation of IAsyncReader.
class TChunkFileReader
    : public IAsyncReader
{
public:
    typedef TIntrusivePtr<TChunkFileReader> TPtr;

    //! Creates a new reader.
    TChunkFileReader(const Stroka& fileName);

    //! Opens the files, reads chunk meta. Must be called before reading blocks.
    void Open();

    //! Returns the meta file size.
    i64 GetMetaSize() const;

    //! Returns the data file size.
    i64 GetDataSize() const;

    //! Returns the full chunk size.
    i64 GetFullSize() const;

    //! Returns the typed chunk info.
    const NChunkHolder::NProto::TChunkInfo& GetChunkInfo() const;

    //! Implements IChunkReader and calls #ReadBlock.
    virtual TAsyncReadResult::TPtr AsyncReadBlocks(const yvector<int>& blockIndexes);

    //! Implements IChunkReader and calls #GetChunkInfo.
    virtual TAsyncGetInfoResult::TPtr AsyncGetChunkInfo();

    //! Synchronously reads a given block from the file.
    /*!
     *  Returns NULL reference if the block does not exist.
     */
    TSharedRef ReadBlock(int blockIndex);

private:
    Stroka FileName;
    bool Opened;
    THolder<TFile> DataFile;
    i64 InfoSize;
    i64 DataSize;
    NChunkHolder::NProto::TChunkInfo ChunkInfo;

};


///////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
