#pragma once

#include "public.h"

#include <server/chunk_server/chunk_owner_base.h>

#include <server/cypress_server/node_detail.h>

namespace NYT {
namespace NJournalServer {

////////////////////////////////////////////////////////////////////////////////

class TJournalNode
    : public NChunkServer::TChunkOwnerBase
{
public:
    DEFINE_BYVAL_RW_PROPERTY(int, ReadQuorum);
    DEFINE_BYVAL_RW_PROPERTY(int, WriteQuorum);

public:
    explicit TJournalNode(const NCypressServer::TVersionedNodeId& id);

    bool IsSealed() const;

};

////////////////////////////////////////////////////////////////////////////////

NCypressServer::INodeTypeHandlerPtr CreateJournalTypeHandler(NCellMaster::TBootstrap* bootstrap);

////////////////////////////////////////////////////////////////////////////////

} // namespace NJournalServer
} // namespace NYT

