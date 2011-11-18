#include "stdafx.h"
#include "orchid_service.h"

#include "../ytree/yson_reader.h"
#include "../ytree/yson_writer.h"
#include "../ytree/ypath_rpc.h"

namespace NYT {
namespace NOrchid {

using namespace NRpc;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

static NLog::TLogger& Logger(OrchidLogger);

////////////////////////////////////////////////////////////////////////////////

TOrchidService::TOrchidService(
    NYTree::INode* root,
    NRpc::IServer* server,
    IInvoker* invoker)
    : NRpc::TServiceBase(
        invoker,
        TOrchidServiceProxy::GetServiceName(),
        OrchidLogger.GetCategory())
    , Root(root)
{
    YASSERT(root != NULL);
    YASSERT(server != NULL);

    RegisterMethod(RPC_SERVICE_METHOD_DESC(Execute));

    server->RegisterService(this);
}

////////////////////////////////////////////////////////////////////////////////

RPC_SERVICE_METHOD_IMPL(TOrchidService, Execute)
{
    UNUSED(response);

    const auto& attachments = request->Attachments();
    YASSERT(attachments.ysize() >= 2);

    TYPath path;
    Stroka verb;
    ParseYPathRequestHeader(
        attachments[0],
        &path,
        &verb);

    context->SetRequestInfo("Path: %s, Verb: %s",
        ~path,
        ~verb);

    auto rootService = IYPathService::FromNode(~Root);

    IYPathService::TPtr suffixService;
    TYPath suffixPath;
    try {
        ResolveYPath(~rootService, path, false, &suffixService, &suffixPath);
    } catch (...) {
        ythrow TServiceException(EErrorCode::ResolutionError) << CurrentExceptionMessage();
    }

    LOG_DEBUG("Execute: SuffixPath: %s", ~suffixPath);

    auto requestMessage = UnwrapYPathRequest(~context->GetUntypedContext());
    auto updatedRequestMessage = UpdateYPathRequestHeader(~requestMessage, suffixPath, verb);
    auto innerContext = CreateYPathContext(
        ~updatedRequestMessage,
        suffixPath,
        verb,
        Logger.GetCategory(),
        ~FromFunctor([=] (const TYPathResponseHandlerParam& param)
            {
                WrapYPathResponse(~context->GetUntypedContext(), ~param.Message);
                context->Reply();
            }));

    suffixService->Invoke(~innerContext);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NOrchid
} // namespace NYT

