#include "stdafx.h"
#include "cypress_integration.h"
#include "node.h"
#include "node_tracker.h"
#include "config.h"

#include <ytlib/ytree/virtual.h>
#include <ytlib/ytree/fluent.h>

#include <ytlib/cypress_client/cypress_ypath_proxy.h>

#include <ytlib/node_tracker_client/node_tracker_service.pb.h>

#include <server/cypress_server/virtual.h>
#include <server/cypress_server/node_proxy_detail.h>

#include <server/chunk_server/chunk_manager.h>

#include <server/cell_master/bootstrap.h>

namespace NYT {
namespace NNodeTrackerServer {

using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NCypressServer;
using namespace NCypressClient;
using namespace NTransactionServer;
using namespace NCellMaster;
using namespace NObjectClient;
using namespace NNodeTrackerClient::NProto;

////////////////////////////////////////////////////////////////////////////////

class TCellNodeProxy
    : public TMapNodeProxy
{
public:
    TCellNodeProxy(
        INodeTypeHandlerPtr typeHandler,
        TBootstrap* bootstrap,
        TTransaction* transaction,
        TMapNode* trunkNode)
        : TMapNodeProxy(
            typeHandler,
            bootstrap,
            transaction,
            trunkNode)
    { }

private:
    TNode* FindNode() const
    {
        auto address = GetParent()->AsMap()->GetChildKey(this);
        auto nodeTracker = Bootstrap->GetNodeTracker();
        return nodeTracker->FindNodeByAddress(address);
    }

    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) override
    {
        const auto* node = FindNode();
        attributes->push_back(TAttributeInfo("state"));
        attributes->push_back(TAttributeInfo("confirmed", node));
        attributes->push_back(TAttributeInfo("statistics", node));
        TMapNodeProxy::ListSystemAttributes(attributes);
    }

    virtual bool GetSystemAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        const auto* node = FindNode();

        if (key == "state") {
            auto state = node ? node->GetState() : ENodeState(ENodeState::Offline);
            BuildYsonFluently(consumer)
                .Value(FormatEnum(state));
            return true;
        }

        if (node) {
            if (key == "confirmed") {
                ValidateActiveLeader();
                BuildYsonFluently(consumer)
                    .Value(FormatBool(node->GetConfirmed()));
                return true;
            }

            if (key == "statistics") {
                const auto& nodeStatistics = node->Statistics();
                BuildYsonFluently(consumer)
                    .BeginMap()
                        .Item("total_available_space").Value(nodeStatistics.total_available_space())
                        .Item("total_used_space").Value(nodeStatistics.total_used_space())
                        .Item("total_chunk_count").Value(nodeStatistics.total_chunk_count())
                        .Item("total_session_count").Value(node->GetTotalSessionCount())
                        .Item("full").Value(nodeStatistics.full())
                        .Item("locations").DoListFor(nodeStatistics.locations(), [] (TFluentList fluent, const TLocationStatistics& locationStatistics) {
                            fluent
                                .Item().BeginMap()
                                    .Item("available_space").Value(locationStatistics.available_space())
                                    .Item("used_space").Value(locationStatistics.used_space())
                                    .Item("chunk_count").Value(locationStatistics.chunk_count())
                                    .Item("session_count").Value(locationStatistics.session_count())
                                    .Item("full").Value(locationStatistics.full())
                                    .Item("enabled").Value(locationStatistics.enabled())
                                .EndMap();
                        })
                    .EndMap();
                return true;
            }
        }

        return TMapNodeProxy::GetSystemAttribute(key, consumer);
    }

    virtual void ValidateUserAttributeUpdate(
        const Stroka& key,
        const TNullable<TYsonString>& oldValue,
        const TNullable<TYsonString>& newValue) override
    {
        UNUSED(oldValue);

        // Update the attributes and check if they still deserialize OK.
        auto attributes = Attributes().Clone();
        if (newValue) {
            attributes->Set(key, *newValue);
        } else {
            attributes->Remove(key);
        }
        ConvertTo<TNodeConfigPtr>(attributes->ToMap());
    }

    virtual void OnUserAttributesUpdated() override
    {
        auto* node = FindNode();
        if (!node)
            return;

        auto nodeTracker = Bootstrap->GetNodeTracker();
        nodeTracker->RefreshNodeConfig(node);
    }
};

class TCellNodeTypeHandler
    : public TMapNodeTypeHandler
{
public:
    explicit TCellNodeTypeHandler(TBootstrap* bootstrap)
        : TMapNodeTypeHandler(bootstrap)
    { }

    virtual EObjectType GetObjectType() override
    {
        return EObjectType::CellNode;
    }

private:
    virtual ICypressNodeProxyPtr DoGetProxy(
        TMapNode* trunkNode,
        TTransaction* transaction) override
    {
        return New<TCellNodeProxy>(
            this,
            Bootstrap,
            transaction,
            trunkNode);
    }
};

INodeTypeHandlerPtr CreateCellNodeTypeHandler(TBootstrap* bootstrap)
{
    YASSERT(bootstrap);

    return New<TCellNodeTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

class TCellNodeMapProxy
    : public TMapNodeProxy
{
public:
    TCellNodeMapProxy(
        INodeTypeHandlerPtr typeHandler,
        TBootstrap* bootstrap,
        TTransaction* transaction,
        TMapNode* trunkNode)
        : TMapNodeProxy(
            typeHandler,
            bootstrap,
            transaction,
            trunkNode)
    { }

private:
    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) override
    {
        attributes->push_back("offline");
        attributes->push_back("registered");
        attributes->push_back("online");
        attributes->push_back("unconfirmed");
        attributes->push_back("confirmed");
        attributes->push_back("available_space");
        attributes->push_back("used_space");
        attributes->push_back("chunk_count");
        attributes->push_back("session_count");
        attributes->push_back("online_node_count");
        attributes->push_back("chunk_replicator_enabled");
        TMapNodeProxy::ListSystemAttributes(attributes);
    }

    virtual bool GetSystemAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        auto nodeTracker = Bootstrap->GetNodeTracker();
        auto chunkManager = Bootstrap->GetChunkManager();

        if (key == "offline") {
            BuildYsonFluently(consumer)
                .DoListFor(GetKeys(), [=] (TFluentList fluent, Stroka address) {
                    if (!nodeTracker->FindNodeByAddress(address)) {
                        fluent.Item().Value(address);
                    }
                });
            return true;
        }

        if (key == "registered" || key == "online") {
            auto expectedState = key == "registered" ? ENodeState::Registered : ENodeState::Online;
            BuildYsonFluently(consumer)
                .DoListFor(nodeTracker->GetNodes(), [=] (TFluentList fluent, TNode* node) {
                    if (node->GetState() == expectedState) {
                        fluent.Item().Value(node->GetAddress());
                    }
                });
            return true;
        }

        if (key == "unconfirmed" || key == "confirmed") {
            ValidateActiveLeader();
            bool expectedConfirmed = key == "confirmed";
            BuildYsonFluently(consumer)
                .DoListFor(nodeTracker->GetNodes(), [=] (TFluentList fluent, TNode* node) {
                    if (node->GetConfirmed() == expectedConfirmed) {
                        fluent.Item().Value(node->GetAddress());
                    }
                });
            return true;
        }

        auto statistics = nodeTracker->GetTotalNodeStatistics();
        if (key == "available_space") {
            BuildYsonFluently(consumer)
                .Value(statistics.AvailbaleSpace);
            return true;
        }

        if (key == "used_space") {
            BuildYsonFluently(consumer)
                .Value(statistics.UsedSpace);
            return true;
        }

        if (key == "chunk_count") {
            BuildYsonFluently(consumer)
                .Value(statistics.ChunkCount);
            return true;
        }

        if (key == "session_count") {
            BuildYsonFluently(consumer)
                .Value(statistics.SessionCount);
            return true;
        }

        if (key == "online_node_count") {
            BuildYsonFluently(consumer)
                .Value(statistics.OnlineNodeCount);
            return true;
        }

        if (key == "chunk_replicator_enabled") {
            ValidateActiveLeader();
            BuildYsonFluently(consumer)
                .Value(chunkManager->IsReplicatorEnabled());
            return true;
        }

        return TMapNodeProxy::GetSystemAttribute(key, consumer);
    }
};

class TNodeMapTypeHandler
    : public TMapNodeTypeHandler
{
public:
    explicit TNodeMapTypeHandler(TBootstrap* bootstrap)
        : TMapNodeTypeHandler(bootstrap)
    { }

    virtual EObjectType GetObjectType() override
    {
        return EObjectType::CellNodeMap;
    }

private:
    virtual ICypressNodeProxyPtr DoGetProxy(
        TMapNode* trunkNode,
        TTransaction* transaction) override
    {
        return New<TCellNodeMapProxy>(
            this,
            Bootstrap,
            transaction,
            trunkNode);
    }

};

INodeTypeHandlerPtr CreateCellNodeMapTypeHandler(TBootstrap* bootstrap)
{
    YCHECK(bootstrap);

    return New<TNodeMapTypeHandler>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerServer
} // namespace NYT
