#pragma once

#include "public.h"

#include <yt/server/controller_agent/public.h>

#include <yt/server/chunk_server/public.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/api/public.h>

#include <yt/core/actions/signal.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

//! Information retrieved during scheduler-master handshake.
struct TMasterHandshakeResult
{
    std::vector<TOperationPtr> Operations;
};

using TWatcherRequester = TCallback<void(NObjectClient::TObjectServiceProxy::TReqExecuteBatchPtr)>;
using TWatcherHandler = TCallback<void(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr)>;

DEFINE_ENUM(EMasterConnectorState,
    (Disconnected)
    (Connecting)
    (Connected)
);

//! Mediates communication between scheduler and master.
/*!
 *  \note Thread affinity: control unless noted otherwise
 */
class TMasterConnector
{
public:
    TMasterConnector(
        TSchedulerConfigPtr config,
        TBootstrap* bootstrap);
    ~TMasterConnector();

    /*!
     *  \note Thread affinity: any
     */
    void Start();

    /*!
     *  \note Thread affinity: any
     */
    EMasterConnectorState GetState() const;

    /*!
     *  \note Thread affinity: any
     */
    TInstant GetConnectionTime() const;

    const NApi::ITransactionPtr& GetLockTransaction() const;

    const IInvokerPtr& GetCancelableControlInvoker(EControlQueue queue = EControlQueue::Default) const;

    void Disconnect(const TError& error);

    void StartOperationNodeUpdates(const TOperationPtr& operation);
    TFuture<void> CreateOperationNode(const TOperationPtr& operation);
    TFuture<void> UpdateInitializedOperationNode(const TOperationPtr& operation);
    TFuture<void> FlushOperationNode(const TOperationPtr& operation);
    TFuture<void> FetchOperationRevivalDescriptors(const std::vector<TOperationPtr>& operations);

    void AttachJobContext(
        const NYPath::TYPath& path,
        const NChunkClient::TChunkId& chunkId,
        const TOperationId& operationId,
        const TJobId& jobId,
        const TString& usrt);

    TFuture<void> FlushOperationRuntimeParameters(
        TOperationPtr operation,
        const TOperationRuntimeParametersPtr& params);

    void SetSchedulerAlert(ESchedulerAlertType alertType, const TError& alert);

    void AddGlobalWatcherRequester(TWatcherRequester requester);
    void AddGlobalWatcherHandler(TWatcherHandler handler);
    void AddGlobalWatcher(TWatcherRequester requester, TWatcherHandler handler, TDuration period);

    void AddOperationWatcherRequester(const TOperationPtr& operation, TWatcherRequester requester);
    void AddOperationWatcherHandler(const TOperationPtr& operation, TWatcherHandler handler);

    void UpdateConfig(const TSchedulerConfigPtr& config);

    //! Raised when connection prcoess starts.
    //! Subscribers may throw and yield.
    DECLARE_SIGNAL(void(), MasterConnecting);

    //! Raised during connection process.
    //! Handshake result contains operations created from Cypress data; all of these have valid revival descriptors.
    //! Subscribers may throw and yield.
    DECLARE_SIGNAL(void(const TMasterHandshakeResult& result), MasterHandshake);

    //! Raised when connection is complete.
    //! Subscribers may throw but cannot yield.
    DECLARE_SIGNAL(void(), MasterConnected);

    //! Raised when disconnect happens.
    //! Subscribers cannot neither throw nor yield
    DECLARE_SIGNAL(void(), MasterDisconnected);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
