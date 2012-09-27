#pragma once

#include "public.h"
#include "chunk_tree_statistics.h"
#include "chunk_tree_ref.h"

#include <ytlib/misc/property.h>

#include <server/cell_master/public.h>

#include <server/object_server/object_detail.h>
#include <server/cypress_server/public.h>

namespace NYT {
namespace NChunkServer {

////////////////////////////////////////////////////////////////////////////////

struct TVersionedChunkListId
{
    TChunkListId Id;
    int Version;

    TVersionedChunkListId(const TChunkListId& id, int version);

};

////////////////////////////////////////////////////////////////////////////////

class TChunkList
    : public NObjectServer::TObjectWithIdBase
{
    DEFINE_BYREF_RW_PROPERTY(std::vector<TChunkTreeRef>, Children);

    // Accumulated sums of children row counts .
    // i-th value is equal to the sum of row counts of children 0..i 
    // for all i in [0..Children.size() - 2]
    // Accumalated statistic for the last child (which is effectively total chunk list statistic)
    // is stored in #Statistics field.
    DEFINE_BYREF_RW_PROPERTY(std::vector<i64>, RowCountSums);
    DEFINE_BYREF_RW_PROPERTY(yhash_multiset<TChunkList*>, Parents);
    DEFINE_BYREF_RW_PROPERTY(TChunkTreeStatistics, Statistics);
    DEFINE_BYREF_RW_PROPERTY(yhash_set<NCypressServer::ICypressNode*>, OwningNodes);
    
    // A tuple of key columns. If empty then the chunk list is not sorted.
    DEFINE_BYREF_RW_PROPERTY(std::vector<Stroka>, SortedBy);

    // If True then the subtree of this chunk list cannot be rebalanced.
    // Rebalancing changes the set of children (while maintaining the set of leaves).
    // For some chunk lists (e.g. those corresponding to roots of branched tables)
    // such changes are not allowed since they would break the invariants.
    DEFINE_BYVAL_RW_PROPERTY(bool, Rigid);

    // Required for optimistic locking during splitted fetch responses.
    DEFINE_BYVAL_RO_PROPERTY(i64, Version);

public:
    explicit TChunkList(const TChunkListId& id);

    void Save(const NCellMaster::TSaveContext& context) const;
    void Load(const NCellMaster::TLoadContext& context);

    void IncreaseVersion();
    TVersionedChunkListId GetVersionedId() const;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkServer
} // namespace NYT