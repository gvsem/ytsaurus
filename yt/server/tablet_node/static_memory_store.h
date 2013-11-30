#pragma once

#include "public.h"
#include "store.h"
#include "static_memory_store_bits.h"

#include <core/misc/chunked_memory_pool.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TStaticMemoryStoreBuilder
{
public:
    TStaticMemoryStoreBuilder(
        TTabletManagerConfigPtr config,
        TTablet* tablet);

    void BeginRow();
    TTimestamp* AllocateTimestamps(int count);
    NVersionedTableClient::TUnversionedValue* AllocateKeys();
    NVersionedTableClient::TVersionedValue* AllocateFixedValues(int index, int count);
    void EndRow(TTimestamp lastCommitTimestamp);

    TStaticMemoryStorePtr Finish();

private:
    friend class TStaticMemoryStore;

    struct TSegment
    {
        TSharedRef Rows;
        int RowCount;
    };

    struct TData
    {
        size_t RowSize;
        std::unique_ptr<TChunkedMemoryPool> AlignedPool;
        std::unique_ptr<TChunkedMemoryPool> UnalignedPool;
        std::vector<TSegment> Segments;
    };

    TTabletManagerConfigPtr Config_;
    TTablet* Tablet_;

    int KeyCount_;
    int SchemaColumnCount_;

    std::unique_ptr<TData> Data_;
    TStaticRow CurrentRow_;


    void CopyValueIfNeeded(NVersionedTableClient::TUnversionedValue* value);

};

////////////////////////////////////////////////////////////////////////////////

class TStaticMemoryStore
    : public IStore
{
public:
    typedef TStaticMemoryStoreBuilder::TData TData;
    typedef TStaticMemoryStoreBuilder::TSegment TSegment;

    TStaticMemoryStore(
        TTabletManagerConfigPtr config,
        TTablet* tablet,
        std::unique_ptr<TData> data);

    virtual std::unique_ptr<IStoreScanner> CreateScanner() override;

private:
    class TScanner;
    friend class TMemoryCompactor;

    TTabletManagerConfigPtr Config_;
    TTablet* Tablet_;
    std::unique_ptr<TData> Data_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
