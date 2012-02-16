#pragma once

#include "id.h"

#include <ytlib/ytree/public.h>

namespace NYT {
namespace NCypress {

struct ICypressNode;
struct ICypressNodeProxy;

////////////////////////////////////////////////////////////////////////////////

//! Describes a behavior object that lives as long as the node
//! exists in Cypress.
/*!
 *  \note
 *  Behaviors are only created at leaders.
 *  Behaviors are only created for non-branched nodes.
 */
struct INodeBehavior
    : public virtual TRefCounted
{
    typedef TIntrusivePtr<INodeBehavior> TPtr;

    //! Called when the node owning the behavior object is about to
    //! be destroyed.
    virtual void Destroy() = 0;
};

////////////////////////////////////////////////////////////////////////////////

//! Provides node type-specific services.
struct INodeTypeHandler
    : public virtual TRefCounted
{
    typedef TIntrusivePtr<INodeTypeHandler> TPtr;

    //! Constructs a proxy.
    /*!
     *  \param transactionId The id of the transaction for which the proxy
     *  is being created (possibly #NullTransactionId).
     *  \return The constructed proxy.
     */
    virtual TIntrusivePtr<ICypressNodeProxy> GetProxy(const TVersionedNodeId& id) = 0;

    //! Returns the (dynamic) node type.
    virtual EObjectType GetObjectType() = 0;
    
    //! Returns the (static) node type.
    virtual NYTree::ENodeType GetNodeType() = 0;

    //! Create a empty instance of the node.
    /*!
     *  This method is called when:
     *  - a static or dynamic node is being created
     *  - a node (possibly dynamic) is being loaded from a snapshot
     */
    virtual TAutoPtr<ICypressNode> Create(const TVersionedNodeId& id) = 0;

    //! Creates and registers dynamic node with a given manifest.
    /*!
     *  This is called during <tt>Create<tt> verb execution.
     */
    virtual void CreateFromManifest(
        const TNodeId& nodeId,
        const TTransactionId& transactionId,
        NYTree::IMapNode* manifest) = 0;

    //! Performs cleanup on node destruction.
    /*!
     *  This is called prior to the actual removal of the node from the meta-map.
     *  A typical implementation will release the resources held by the node,
     *  decrement the ref-counters of the children etc.
     *  
     *  \note This method is only called for committed and uncommitted nodes.
     *  It is not called for branched ones.
     */
    virtual void Destroy(ICypressNode& node) = 0;

    //! Returns True if the given locking mode is supported.
    virtual bool IsLockModeSupported(ELockMode mode) = 0;

    //! Branches a node into a given transaction.
    /*!
     *  \param node The originating node.
     *  \param transactionId The id of the transaction that needs a copy of the node.
     *  \returns The branched node.
     */
    virtual TAutoPtr<ICypressNode> Branch(
        const ICypressNode& node,
        const TTransactionId& transactionId,
        ELockMode mode) = 0;

    //! Merges the changes made in the branched node back into the committed one.
    /*!
     *  \param branchedNode The branched node.
     *
     *  \note 
     *  #branchedNode is non-const for performance reasons (i.e. to swap the data instead of copying).
     */
    virtual void Merge(
        ICypressNode& originatingNode,
        ICypressNode& branchedNode) = 0;

    //! Creates a behavior object.
    /*!
     *  \note
     *  The method may return NULL if no behavior is needed.
     *  
     *  The implementation must not keep the node reference since node's
     *  content may get eventually destroyed by TMetaStateMap.
     *  It should keep node id instead.
     */
    virtual INodeBehavior::TPtr CreateBehavior(const TNodeId& id) = 0;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypress
} // namespace NYT
