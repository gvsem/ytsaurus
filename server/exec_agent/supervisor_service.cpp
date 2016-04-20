#include "supervisor_service.h"
#include "private.h"
#include "job.h"
#include "supervisor_service_proxy.h"

#include <yt/server/cell_node/bootstrap.h>

#include <yt/server/job_agent/job_controller.h>
#include <yt/server/job_agent/public.h>

#include <yt/ytlib/node_tracker_client/helpers.h>

#include <yt/core/misc/common.h>

namespace NYT {
namespace NExecAgent {

using namespace NJobAgent;
using namespace NNodeTrackerClient;
using namespace NCellNode;

////////////////////////////////////////////////////////////////////////////////

TSupervisorService::TSupervisorService(TBootstrap* bootstrap)
    : NRpc::TServiceBase(
        bootstrap->GetControlInvoker(),
        TSupervisorServiceProxy::GetServiceName(),
        ExecAgentLogger)
    , Bootstrap(bootstrap)
{
    RegisterMethod(
        RPC_SERVICE_METHOD_DESC(GetJobSpec)
        .SetResponseCodec(NCompression::ECodec::Lz4)
        .SetResponseHeavy(true));
    RegisterMethod(RPC_SERVICE_METHOD_DESC(OnJobFinished));
    RegisterMethod(RPC_SERVICE_METHOD_DESC(OnJobProgress)
        .SetOneWay(true));
    RegisterMethod(RPC_SERVICE_METHOD_DESC(UpdateResourceUsage)
        .SetOneWay(true));
}

DEFINE_RPC_SERVICE_METHOD(TSupervisorService, GetJobSpec)
{
    auto jobId = FromProto<TJobId>(request->job_id());
    context->SetRequestInfo("JobId: %v", jobId);

    auto jobController = Bootstrap->GetJobController();
    auto job = jobController->GetJobOrThrow(jobId);

    *response->mutable_job_spec() = job->GetSpec();
    *response->mutable_resource_usage() = job->GetResourceUsage();

    context->Reply();
}

DEFINE_RPC_SERVICE_METHOD(TSupervisorService, OnJobFinished)
{
    auto jobId = FromProto<TJobId>(request->job_id());
    const auto& result = request->result();
    auto error = FromProto<TError>(result.error());
    context->SetRequestInfo("JobId: %v, Error: %v",
        jobId,
        error);

    auto jobController = Bootstrap->GetJobController();
    auto job = jobController->GetJobOrThrow(jobId);

    job->SetResult(result);

    context->Reply();
}

DEFINE_ONE_WAY_RPC_SERVICE_METHOD(TSupervisorService, OnJobProgress)
{
    auto jobId = FromProto<TJobId>(request->job_id());
    double progress = request->progress();
    const auto& statistics = request->statistics();

    context->SetRequestInfo("JobId: %v, Progress: %lf, Statistics: %Qv",
        jobId,
        progress,
        statistics.DebugString());

    auto jobController = Bootstrap->GetJobController();
    auto job = jobController->GetJobOrThrow(jobId);

    job->SetProgress(progress);
    job->SetStatistics(statistics);
}

DEFINE_ONE_WAY_RPC_SERVICE_METHOD(TSupervisorService, UpdateResourceUsage)
{
    auto jobId = FromProto<TJobId>(request->job_id());
    const auto& resourceUsage = request->resource_usage();

    context->SetRequestInfo("JobId: %v, ResourceUsage: {%v}",
        jobId,
        FormatResources(resourceUsage));

    auto jobController = Bootstrap->GetJobController();
    auto job = jobController->GetJobOrThrow(jobId);

    job->SetResourceUsage(resourceUsage);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NExecAgent
} // namespace NYT
