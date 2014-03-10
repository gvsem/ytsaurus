#pragma once

#include "public.h"
#include "partition.h"

#include <core/misc/property.h>
#include <core/misc/ref_tracked.h>

#include <core/actions/cancelable_context.h>

#include <ytlib/new_table_client/schema.h>
#include <ytlib/new_table_client/unversioned_row.h>

#include <ytlib/tablet_client/public.h>

#include <ytlib/chunk_client/public.h>

namespace NYT {
namespace NTabletNode {

////////////////////////////////////////////////////////////////////////////////

class TTablet
    : public TRefTracked<TTablet>
{
public:
    DEFINE_BYVAL_RO_PROPERTY(TTabletId, Id);
    DEFINE_BYVAL_RO_PROPERTY(TTabletSlot*, Slot);
    
    DEFINE_BYREF_RO_PROPERTY(NVersionedTableClient::TTableSchema, Schema);
    DEFINE_BYREF_RO_PROPERTY(NVersionedTableClient::TKeyColumns, KeyColumns);
    
    DEFINE_BYVAL_RO_PROPERTY(NVersionedTableClient::TOwningKey, PivotKey);
    DEFINE_BYVAL_RO_PROPERTY(NVersionedTableClient::TOwningKey, NextPivotKey);
    
    DEFINE_BYVAL_RW_PROPERTY(ETabletState, State);

    DEFINE_BYVAL_RO_PROPERTY(TCancelableContextPtr, CancelableContext);

public:
    explicit TTablet(const TTabletId& id);
    TTablet(
        NTabletClient::TTableMountConfigPtr config,
        TTabletWriterOptionsPtr writerOptions,
        const TTabletId& id,
        TTabletSlot* slot,
        const NVersionedTableClient::TTableSchema& schema,
        const NVersionedTableClient::TKeyColumns& keyColumns,
        NVersionedTableClient::TOwningKey pivotKey,
        NVersionedTableClient::TOwningKey nextPivotKey);

    ~TTablet();

    const NTabletClient::TTableMountConfigPtr& GetConfig();
    const TTabletWriterOptionsPtr& GetWriterOptions();

    const TStoreManagerPtr& GetStoreManager() const;
    void SetStoreManager(TStoreManagerPtr manager);

    typedef std::vector<std::unique_ptr<TPartition>> TPartitionList;
    typedef TPartitionList::iterator TPartitionListIterator;

    const TPartitionList& Partitions() const;
    TPartition* GetEden() const;
    TPartition* AddPartition(NVersionedTableClient::TOwningKey pivotKey);
    TPartition* FindPartitionByPivotKey(const NVersionedTableClient::TOwningKey& pivotKey);
    TPartition* GetPartitionByPivotKey(const NVersionedTableClient::TOwningKey& pivotKey);
    void MergePartitions(int firstIndex, int lastIndex);
    void SplitPartition(int index, const std::vector<NVersionedTableClient::TOwningKey>& pivotKeys);

    //! Finds a partition fully containing the range |[minKey, maxKey]|.
    //! Returns the Eden if no such partition exists.
    TPartition* GetContainingPartition(
        const NVersionedTableClient::TOwningKey& minKey,
        const NVersionedTableClient::TOwningKey& maxKey);

    //! Returns a range of partitions intersecting with the range |[lowerBound, uppwerBound)|.
    std::pair<TPartitionListIterator, TPartitionListIterator> GetIntersectingPartitions(
        const NVersionedTableClient::TOwningKey& lowerBound,
        const NVersionedTableClient::TOwningKey& upperBound);

    const yhash_map<TStoreId, IStorePtr>& Stores() const;
    void AddStore(IStorePtr store);
    void RemoveStore(const TStoreId& id);
    IStorePtr FindStore(const TStoreId& id);
    IStorePtr GetStore(const TStoreId& id);

    const TDynamicMemoryStorePtr& GetActiveStore() const;
    void SetActiveStore(TDynamicMemoryStorePtr store);

    void Save(TSaveContext& context) const;
    void Load(TLoadContext& context);

    int GetSchemaColumnCount() const;
    int GetKeyColumnCount() const;

    void StartEpoch(TTabletSlotPtr slot);
    void StopEpoch();
    IInvokerPtr GetEpochAutomatonInvoker(EAutomatonThreadQueue queue);

private:
    NTabletClient::TTableMountConfigPtr Config_;
    TTabletWriterOptionsPtr WriterOptions_;

    TStoreManagerPtr StoreManager_;

    std::vector<IInvokerPtr> EpochAutomatonInvokers_;

    std::unique_ptr<TPartition> Eden_;

    TPartitionList  Partitions_;

    yhash_map<TStoreId, IStorePtr> Stores_;
    TDynamicMemoryStorePtr ActiveStore_;


    TPartition* GetContainingPartition(IStorePtr store);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTabletNode
} // namespace NYT
