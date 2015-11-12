#include "object_service.h"
#include "private.h"
#include "config.h"
#include "object_manager.h"

#include <yt/server/cell_master/bootstrap.h>
#include <yt/server/cell_master/hydra_facade.h>
#include <yt/server/cell_master/master_hydra_service.h>

#include <yt/server/cypress_server/cypress_manager.h>

#include <yt/server/security_server/security_manager.h>
#include <yt/server/security_server/user.h>

#include <yt/server/transaction_server/transaction.h>
#include <yt/server/transaction_server/transaction_manager.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/security_client/public.h>

#include <yt/core/misc/common.h>

#include <yt/core/rpc/helpers.h>
#include <yt/core/rpc/message.h>
#include <yt/core/rpc/service_detail.h>

#include <yt/core/ytree/ypath_detail.h>

#include <atomic>

namespace NYT {
namespace NObjectServer {

using namespace NHydra;
using namespace NRpc;
using namespace NRpc::NProto;
using namespace NBus;
using namespace NYTree;
using namespace NYTree::NProto;
using namespace NCypressClient;
using namespace NCypressServer;
using namespace NTransactionServer;
using namespace NSecurityClient;
using namespace NSecurityServer;
using namespace NObjectServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TObjectService)

class TObjectService
    : public NCellMaster::TMasterHydraServiceBase
{
public:
    explicit TObjectService(TBootstrap* bootstrap)
        : TMasterHydraServiceBase(
            bootstrap,
            NObjectClient::TObjectServiceProxy::GetServiceName(),
            ObjectServerLogger,
            NObjectClient::TObjectServiceProxy::GetProtocolVersion())
    {
        RegisterMethod(RPC_SERVICE_METHOD_DESC(Execute));
        RegisterMethod(RPC_SERVICE_METHOD_DESC(GCCollect));
    }

private:
    class TExecuteSession;

    DECLARE_RPC_SERVICE_METHOD(NObjectClient::NProto, Execute);
    DECLARE_RPC_SERVICE_METHOD(NObjectClient::NProto, GCCollect);

};

DEFINE_REFCOUNTED_TYPE(TObjectService)

IServicePtr CreateObjectService(TBootstrap* bootstrap)
{
    return New<TObjectService>(bootstrap);
}

////////////////////////////////////////////////////////////////////////////////

class TObjectService::TExecuteSession
    : public TIntrinsicRefCounted
{
public:
    TExecuteSession(
        TObjectServicePtr owner,
        TCtxExecutePtr context)
        : Owner(std::move(owner))
        , Context(std::move(context))
        , RequestCount(Context->Request().part_counts_size())
    { }

    void Run()
    {
        Context->SetRequestInfo("RequestCount: %v", RequestCount);

        if (RequestCount == 0) {
            Reply();
            return;
        }

        ResponseMessages.resize(RequestCount);
        RequestHeaders.resize(RequestCount);
        UserName = Context->GetUser();

        auto hydraManager = Owner->Bootstrap_->GetHydraFacade()->GetHydraManager();
        auto sync = hydraManager->SyncWithLeader();
        if (sync.IsSet()) {
            OnSync(sync.Get());
        } else {
            sync.Subscribe(BIND(&TExecuteSession::OnSync, MakeStrong(this))
                .Via(GetCurrentInvoker()));
        }
    }

private:
    const TObjectServicePtr Owner;
    const TCtxExecutePtr Context;

    const int RequestCount;

    TFuture<void> LastMutationCommitted = VoidFuture;
    std::atomic<bool> Replied = {false};
    std::atomic<int> ResponseCount = {0};
    std::vector<TSharedRefArray> ResponseMessages;
    std::vector<TRequestHeader> RequestHeaders;
    int CurrentRequestIndex = 0;
    int CurrentRequestPartIndex = 0;
    Stroka UserName;

    const NLogging::TLogger& Logger = ObjectServerLogger;


    void OnSync(const TError& error)
    {
        if (!error.IsOK()) {
            Reply(error);
            return;
        }

        auto* user = GetAuthenticatedUser();
        auto securityManager = Owner->Bootstrap_->GetSecurityManager();
        securityManager->ValidateUserAccess(user, RequestCount);

        Continue();
    }

    void Continue()
    {
        try {
            auto objectManager = Owner->Bootstrap_->GetObjectManager();
            auto rootService = objectManager->GetRootService();

            auto hydraFacade = Owner->Bootstrap_->GetHydraFacade();
            auto hydraManager = hydraFacade->GetHydraManager();

            auto startTime = TInstant::Now();

            auto& request = Context->Request();
            const auto& attachments = request.Attachments();

            auto securityManager = Owner->Bootstrap_->GetSecurityManager();
            auto* user = GetAuthenticatedUser();
            TAuthenticatedUserGuard userGuard(securityManager, user);

            while (CurrentRequestIndex < request.part_counts_size()) {
                // Don't allow the thread to be blocked for too long by a single batch.
                if (objectManager->AdviceYield(startTime)) {
                    hydraFacade->GetEpochAutomatonInvoker()->Invoke(
                        BIND(&TExecuteSession::Continue, MakeStrong(this)));
                    return;
                }

                int partCount = request.part_counts(CurrentRequestIndex);
                if (partCount == 0) {
                    // Skip empty requests.
                    OnResponse(
                        CurrentRequestIndex,
                        false,
                        NTracing::TTraceContext(),
                        nullptr,
                        TSharedRefArray());
                    NextRequest();
                    continue;
                }

                std::vector<TSharedRef> requestParts(
                    attachments.begin() + CurrentRequestPartIndex,
                    attachments.begin() + CurrentRequestPartIndex + partCount);

                auto requestMessage = TSharedRefArray(std::move(requestParts));

                auto& requestHeader = RequestHeaders[CurrentRequestIndex];
                if (!ParseRequestHeader(requestMessage, &requestHeader)) {
                    THROW_ERROR_EXCEPTION(
                        NRpc::EErrorCode::ProtocolError,
                        "Error parsing request header");
                }

                // Propagate various parameters to the subrequest.
                requestHeader.set_retry(requestHeader.retry() || Context->IsRetry());
                requestHeader.set_user(user->GetName());
                auto updatedRequestMessage = SetRequestHeader(requestMessage, requestHeader);

                const auto& ypathExt = requestHeader.GetExtension(TYPathHeaderExt::ypath_header_ext);
                const auto& path = ypathExt.path();
                bool mutating = ypathExt.mutating();

                if (IsBarrierNeeded(mutating) && !LastMutationCommitted.IsSet()) {
                    LastMutationCommitted.Subscribe(
                        BIND(&TExecuteSession::OnLastMutationCommitted, MakeStrong(this))
                            .Via(hydraFacade->GetEpochAutomatonInvoker()));
                    return;
                }

                NTracing::TTraceContextGuard traceContextGuard(NTracing::CreateChildTraceContext());
                NTracing::TraceEvent(
                    requestHeader.service(),
                    requestHeader.method(),
                    NTracing::ServerReceiveAnnotation);

                auto requestInfo = Format("RequestId: %v, Mutating: %v, RequestPath: %v, User: %v",
                    Context->GetRequestId(),
                    mutating,
                    path,
                    UserName);
                auto responseInfo = Format("RequestId: %v",
                    Context->GetRequestId());

                TFuture<TSharedRefArray> asyncResponseMessage;
                try {
                    asyncResponseMessage = ExecuteVerb(
                        rootService,
                        std::move(updatedRequestMessage),
                        ObjectServerLogger,
                        NLogging::ELogLevel::Debug,
                        requestInfo,
                        responseInfo);
                } catch (const TLeaderFallbackException&) {
                    asyncResponseMessage = objectManager->ForwardToLeader(
                        requestMessage,
                        Context->GetTimeout());
                }

                // Optimize for the (typical) case of synchronous response.
                if (asyncResponseMessage.IsSet() && !objectManager->AdviceYield(startTime)) {
                    OnResponse(
                        CurrentRequestIndex,
                        mutating,
                        traceContextGuard.GetContext(),
                        &requestHeader,
                        asyncResponseMessage.Get());
                } else {
                    LastMutationCommitted = asyncResponseMessage.Apply(BIND(
                        &TExecuteSession::OnResponse,
                        MakeStrong(this),
                        CurrentRequestIndex,
                        mutating,
                        traceContextGuard.GetContext(),
                        &requestHeader));
                }

                NextRequest();
            }
        } catch (const std::exception& ex) {
            Reply(ex);
        }
    }

    bool IsBarrierNeeded(bool mutating)
    {
        if (mutating) {
            // Always place a barrier before each write request when doing leader forwarding
            // since the forwarded requests may be served out of order.
            auto hydraFacade = Owner->Bootstrap_->GetHydraFacade();
            auto hydraManager = hydraFacade->GetHydraManager();
            return hydraManager->IsFollower();
        } else {
            // Forbid to reorder read requests before write ones.
            return true;
        }
    }

    void OnLastMutationCommitted(const TError& error)
    {
        if (!error.IsOK()) {
            Reply(error);
            return;
        }

        Continue();
    }

    void OnResponse(
        int requestIndex,
        bool mutating,
        const NTracing::TTraceContext& traceContext,
        const TRequestHeader* requestHeader,
        const TErrorOr<TSharedRefArray>& responseMessageOrError)
    {
        VERIFY_THREAD_AFFINITY_ANY();

        if (!responseMessageOrError.IsOK()) {
            // Unexpected error.
            Context->Reply(responseMessageOrError);
            return;
        }

        const auto& responseMessage = responseMessageOrError.Value();
        if (responseMessage) {
            NTracing::TraceEvent(
                traceContext,
                requestHeader->service(),
                requestHeader->method(),
                NTracing::ServerSendAnnotation);
        }

        ResponseMessages[requestIndex] = std::move(responseMessage);

        if (++ResponseCount == ResponseMessages.size()) {
            Reply();
        }
    }

    void NextRequest()
    {
        const auto& request = Context->Request();
        CurrentRequestPartIndex += request.part_counts(CurrentRequestIndex);
        CurrentRequestIndex += 1;
    }

    void Reply(const TError& error = TError())
    {
        bool expected = false;
        if (!Replied.compare_exchange_strong(expected, true))
            return;

        if (error.IsOK()) {
            auto& response = Context->Response();
            for (const auto& responseMessage : ResponseMessages) {
                if (responseMessage) {
                    response.add_part_counts(responseMessage.Size());
                    response.Attachments().insert(
                        response.Attachments().end(),
                        responseMessage.Begin(),
                        responseMessage.End());
                } else {
                    response.add_part_counts(0);
                }
            }
        }
     
        Context->Reply(error);
    }

    TUser* GetAuthenticatedUser()
    {
        auto securityManager = Owner->Bootstrap_->GetSecurityManager();
        return securityManager->GetUserByNameOrThrow(UserName);
    }

};

////////////////////////////////////////////////////////////////////////////////

DEFINE_RPC_SERVICE_METHOD(TObjectService, Execute)
{
    UNUSED(request);
    UNUSED(response);

    ValidatePeer(EPeerKind::LeaderOrFollower);

    New<TExecuteSession>(this, std::move(context))->Run();
}

DEFINE_RPC_SERVICE_METHOD(TObjectService, GCCollect)
{
    UNUSED(request);
    UNUSED(response);

    ValidatePeer(EPeerKind::Leader);

    context->SetRequestInfo();

    auto objectManager = Bootstrap_->GetObjectManager();
    context->ReplyFrom(objectManager->GCCollect());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
