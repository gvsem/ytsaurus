#pragma once

#include "common.h"
#include "node.h"
#include "node_proxy.h"
#include "lock.h"
#include "cypress_manager.pb.h"

#include "../misc/thread_affinity.h"
#include "../transaction_manager/transaction.h"
#include "../transaction_manager/transaction_manager.h"
#include "../ytree/ypath.h"
#include "../ytree/tree_builder.h"
#include "../misc/id_generator.h"
#include "../meta_state/meta_state_manager.h"
#include "../meta_state/composite_meta_state.h"
#include "../meta_state/map.h"
#include "../meta_state/meta_change.h"

namespace NYT {
namespace NCypress {

using NTransaction::TTransaction;
using NTransaction::NullTransactionId;
using NTransaction::TTransactionManager;
using NMetaState::TMetaChange;

struct ICypressNodeProxy;

////////////////////////////////////////////////////////////////////////////////

class TCypressManager
    : public NMetaState::TMetaStatePart
{
public:
    typedef TCypressManager TThis;
    typedef TIntrusivePtr<TThis> TPtr;

    TCypressManager(
        NMetaState::TMetaStateManager* metaStateManager,
        NMetaState::TCompositeMetaState* metaState,
        TTransactionManager* transactionManager);

    void RegisterNodeType(INodeTypeHandler* handler);

    bool IsWorldInitialized();

    METAMAP_ACCESSORS_DECL(Node, ICypressNode, TBranchedNodeId);

    const ICypressNode* FindTransactionNode(
        const TNodeId& nodeId,
        const TTransactionId& transactionId);

    const ICypressNode& GetTransactionNode(
        const TNodeId& nodeId,
        const TTransactionId& transactionId);

    ICypressNode* FindTransactionNodeForUpdate(
        const TNodeId& nodeId,
        const TTransactionId& transactionId);

    ICypressNode& GetTransactionNodeForUpdate(
        const TNodeId& nodeId,
        const TTransactionId& transactionId);

    TIntrusivePtr<ICypressNodeProxy> GetNodeProxy(
        const TNodeId& nodeId,
        const TTransactionId& transactionId);

    bool IsTransactionNodeLocked(
        const TNodeId& nodeId,
        const TTransactionId& transactionId);

    TLockId LockTransactionNode(
        const TNodeId& nodeId,
        const TTransactionId& transactionId);

    void RefNode(ICypressNode& node);
    void RefNode(const TNodeId& nodeId);
    void UnrefNode(ICypressNode & node);
    void UnrefNode(const TNodeId& nodeId);

    NYTree::IStringNode::TPtr CreateStringNodeProxy(const TTransactionId& transactionId);
    NYTree::IInt64Node::TPtr  CreateInt64NodeProxy(const TTransactionId& transactionId);
    NYTree::IDoubleNode::TPtr CreateDoubleNodeProxy(const TTransactionId& transactionId);
    NYTree::IMapNode::TPtr    CreateMapNodeProxy(const TTransactionId& transactionId);
    NYTree::IListNode::TPtr   CreateListNodeProxy(const TTransactionId& transactionId);

    TAutoPtr<NYTree::ITreeBuilder> GetDeserializationBuilder(const TTransactionId& transactionId);

    NYTree::INode::TPtr CreateDynamicNode(
        const TTransactionId& transactionId,
        NYTree::IMapNode* manifest);

    METAMAP_ACCESSORS_DECL(Lock, TLock, TLockId);

    TLock& CreateLock(const TNodeId& nodeId, const TTransactionId& transactionId);

    ICypressNode& BranchNode(ICypressNode& node, const TTransactionId& transactionId);

    void GetYPath(
        const TTransactionId& transactionId,
        NYTree::TYPath path,
        NYTree::IYsonConsumer* consumer);

    NYTree::INode::TPtr NavigateYPath(
        const TTransactionId& transactionId,
        NYTree::TYPath path);

    TMetaChange<TNodeId>::TPtr InitiateSetYPath(
        const TTransactionId& transactionId,
        NYTree::TYPath path,
        const Stroka& value);

    TMetaChange<TVoid>::TPtr InitiateRemoveYPath(
        const TTransactionId& transactionId,
        NYTree::TYPath path);

    TMetaChange<TVoid>::TPtr InitiateLockYPath(
        const TTransactionId& transactionId,
        NYTree::TYPath path);

    TMetaChange<TVoid>::TPtr InitiateCreateWorld();

private:
    class TNodeMapTraits
    {
    public:
        TNodeMapTraits(TCypressManager* cypressManager);

        TAutoPtr<ICypressNode> Clone(ICypressNode* value) const;
        void Save(ICypressNode* value, TOutputStream* output) const;
        TAutoPtr<ICypressNode> Load(TInputStream* input) const;

    private:
        TCypressManager::TPtr CypressManager;

    };
    
    TTransactionManager::TPtr TransactionManager;

    TIdGenerator<TNodeId> NodeIdGenerator;
    NMetaState::TMetaStateMap<TBranchedNodeId, ICypressNode, TNodeMapTraits> NodeMap;

    TIdGenerator<TLockId> LockIdGenerator; 
    NMetaState::TMetaStateMap<TLockId, TLock> LockMap;

    yvector<INodeTypeHandler::TPtr> RuntimeTypeToHandler;
    yhash_map<Stroka, INodeTypeHandler::TPtr> TypeNameToHandler;

    TNodeId SetYPath(const NProto::TMsgSet& message);
    TVoid RemoveYPath(const NProto::TMsgRemove& message);
    TVoid LockYPath(const NProto::TMsgLock& message);
    TVoid CreateWorld(const NProto::TMsgCreateWorld& message);

    // TMetaStatePart overrides.
    virtual Stroka GetPartName() const;
    virtual TFuture<TVoid>::TPtr Save(TOutputStream* stream, IInvoker::TPtr invoker);
    virtual TFuture<TVoid>::TPtr Load(TInputStream* stream, IInvoker::TPtr invoker);
    virtual void Clear();

    void OnTransactionCommitted(const TTransaction& transaction);
    void OnTransactionAborted(const TTransaction& transaction);

    void ReleaseLocks(const TTransaction& transaction);
    void MergeBranchedNodes(const TTransaction& transaction);
    void RemoveBranchedNodes(const TTransaction& transaction);
    void UnrefOriginatingNodes(const TTransaction& transaction);
    void CommitCreatedNodes(const TTransaction& transaction);

    INodeTypeHandler::TPtr GetTypeHandler(const ICypressNode& node);
    INodeTypeHandler::TPtr GetTypeHandler(ERuntimeNodeType type);

    template <class TImpl, class TProxy>
    TIntrusivePtr<TProxy> CreateNode(
        const TTransactionId& transactionId,
        ERuntimeNodeType type);

    class TDeserializationBuilder;
    friend class TDeserializationBuilder;

    DECLARE_THREAD_AFFINITY_SLOT(StateThread);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypress
} // namespace NYT
