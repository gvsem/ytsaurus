#pragma once

#include "transaction_manager.h"

#include <yt/yt/ytlib/sequoia_client/public.h>

// TODO(kvk1920): move into separate lib.
// NB: it's intermediate PR with reduced diff to ease reading.
namespace NYT::NSequoiaServer {

////////////////////////////////////////////////////////////////////////////////

//! Starts Cypress transaction on a given cell.
/*!
 *  NB: modifies #request.
 */
TFuture<NTransactionClient::TTransactionId> StartCypressTransaction(
    NSequoiaClient::ISequoiaClientPtr sequoiaClient,
    NObjectClient::TCellId cypressTransactionCoordinatorCellId,
    NCypressTransactionClient::NProto::TReqStartTransaction* request,
    NRpc::TAuthenticationIdentity authenticationIdentity,
    IInvokerPtr invoker,
    NLogging::TLogger logger);

//! Aborts Cypress transaction when abort is requested by user.
/*!
 *  NB: modifies #request.
 */
TFuture<void> AbortCypressTransaction(
    NSequoiaClient::ISequoiaClientPtr sequoiaClient,
    NObjectClient::TCellId cypressTransactionCoordinatorCellId,
    NCypressTransactionClient::NProto::TReqAbortTransaction* request,
    NRpc::TAuthenticationIdentity authenticationIdentity,
    IInvokerPtr invoker,
    NLogging::TLogger logger);

//! Aborts expired Cypress transaction. Similar to |AbortCypressTransaction()|,
//! but log message is different.
TFuture<void> AbortExpiredCypressTransaction(
    NSequoiaClient::ISequoiaClientPtr sequoiaClient,
    NObjectClient::TCellId cypressTransactionCoordinatorCellId,
    NTransactionClient::TTransactionId transactionId,
    IInvokerPtr invoker,
    NLogging::TLogger logger);

//! Commits Cypress transactions.
/*!
 *  Note that commit timestamp has to be generated _before_ tx commit. Of
 *  course, it can lead to commit reordering, but it doesn't matter here: the
 *  only known usage of Cypress tx's commit timestamp is bulk insert, which
 *  needs some timestamp before tx's commit but after every action under the
 *  given Cypress tx.
 *
 *  NB: modifies #request.
 */
TFuture<void> CommitCypressTransaction(
    NSequoiaClient::ISequoiaClientPtr sequoiaClient,
    NObjectClient::TCellId cypressTransactionCoordinatorCellId,
    NTransactionClient::TTransactionId transactionId,
    std::vector<NTransactionClient::TTransactionId> prerequisiteTransactionIds,
    NTransactionClient::TTimestamp commitTimestamp,
    NRpc::TAuthenticationIdentity authenticationIdentity,
    IInvokerPtr invoker,
    NLogging::TLogger logger);

////////////////////////////////////////////////////////////////////////////////

// NB: the common case is the lazy replication from transaction coordinator
// which is initiated on foreign cell. In this case destination cell is the only
// destination, thus typical count is 1.
constexpr int TypicalTransactionReplicationDestinationCellCount = 1;
using TTransactionReplicationDestinationCellTagList =
    TCompactVector<NObjectClient::TCellTag, TypicalTransactionReplicationDestinationCellCount>;

//! Checks that given Cypress transactions are replicated to the cell and
//! registers Sequoia tx actions if needed. Returns future which is set when all
//! necessary checks are performed and Sequoia transaction is committed.
TFuture<void> ReplicateCypressTransactions(
    NSequoiaClient::ISequoiaClientPtr sequoiaClient,
    std::vector<NTransactionClient::TTransactionId> transactionIds,
    TTransactionReplicationDestinationCellTagList destinationCellTags,
    NObjectClient::TCellId hintCoordinatorCellId,
    IInvokerPtr invoker,
    NLogging::TLogger logger);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSequoiaServer

namespace NYT::NTransactionServer {

////////////////////////////////////////////////////////////////////////////////

// NB: modifies original RPC request.
void StartCypressTransactionInSequoiaAndReply(
    NCellMaster::TBootstrap* bootstrap,
    const ITransactionManager::TCtxStartCypressTransactionPtr& context);

void AbortCypressTransactionInSequoiaAndReply(
    NCellMaster::TBootstrap* bootstrap,
    const ITransactionManager::TCtxAbortCypressTransactionPtr& context);

TFuture<TSharedRefArray> AbortExpiredCypressTransactionInSequoia(
    NCellMaster::TBootstrap* bootstrap,
    TTransactionId transactionId);

TFuture<TSharedRefArray> CommitCypressTransactionInSequoia(
    NCellMaster::TBootstrap* bootstrap,
    TTransactionId transactionId,
    std::vector<TTransactionId> prerequisiteTransactionIds,
    TTimestamp commitTimestamp,
    NRpc::TAuthenticationIdentity authenticationIdentity);

//! Replicates given Cypress transactions from coordinator to this cell.
TFuture<void> ReplicateCypressTransactionsInSequoiaAndSyncWithLeader(
    NCellMaster::TBootstrap* bootstrap,
    std::vector<TTransactionId> transactionIds);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTransactionServer
