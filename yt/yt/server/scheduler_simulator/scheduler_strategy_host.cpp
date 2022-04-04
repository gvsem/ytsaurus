#include "scheduler_strategy_host.h"

#include "event_log.h"

#include <yt/yt/ytlib/scheduler/job_resources_helpers.h>

namespace NYT::NSchedulerSimulator {

using namespace NScheduler;
using namespace NConcurrency;
using namespace NEventLog;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = SchedulerSimulatorLogger;

////////////////////////////////////////////////////////////////////////////////

TSchedulerStrategyHost::TSchedulerStrategyHost(
    const std::vector<NScheduler::TExecNodePtr>* execNodes,
    IOutputStream* eventLogOutputStream,
    const TRemoteEventLogConfigPtr& remoteEventLogConfig,
    const IInvokerPtr& nodeShardsInvoker)
    : ExecNodes_(execNodes)
    , MediumDirectory_(CreateDefaultMediumDirectory())
    , NodeShardsInvokers_({nodeShardsInvoker})
{
    YT_VERIFY(eventLogOutputStream || remoteEventLogConfig);

    if (remoteEventLogConfig) {
        RemoteEventLogWriter_ = CreateRemoteEventLogWriter(remoteEventLogConfig, GetCurrentInvoker());
        RemoteEventLogConsumer_ = RemoteEventLogWriter_->CreateConsumer();
    } else {
        LocalEventLogWriter_.emplace(eventLogOutputStream, NYson::EYsonFormat::Binary, NYson::EYsonType::ListFragment);
    }

    for (const auto& execNode : *ExecNodes_) {
        TotalResourceLimits_ += execNode->GetResourceLimits();
    }
}

IInvokerPtr TSchedulerStrategyHost::GetControlInvoker(NYT::NScheduler::EControlQueue /*queue*/) const
{
    return GetCurrentInvoker();
}

IInvokerPtr TSchedulerStrategyHost::GetFairShareLoggingInvoker() const
{
    return GetCurrentInvoker();
}

IInvokerPtr TSchedulerStrategyHost::GetFairShareProfilingInvoker() const
{
    return GetCurrentInvoker();
}

IInvokerPtr TSchedulerStrategyHost::GetFairShareUpdateInvoker() const
{
    return GetCurrentInvoker();
}
    
IInvokerPtr TSchedulerStrategyHost::GetBackgroundInvoker() const
{
    return GetCurrentInvoker();
}

IInvokerPtr TSchedulerStrategyHost::GetOrchidWorkerInvoker() const
{
    return GetCurrentInvoker();
}

const std::vector<IInvokerPtr>& TSchedulerStrategyHost::GetNodeShardInvokers() const
{
    return NodeShardsInvokers_;
}

TFluentLogEvent TSchedulerStrategyHost::LogFairShareEventFluently(TInstant now)
{
    return LogEventFluently(GetEventLogger(), ELogEventType::FairShareInfo, now);
}
    
TFluentLogEvent TSchedulerStrategyHost::LogAccumulatedUsageEventFluently(TInstant /*now*/)
{
    YT_UNIMPLEMENTED();
}

TJobResources TSchedulerStrategyHost::GetResourceLimits(const TSchedulingTagFilter& filter) const
{
    auto it = FilterToJobResources_.find(filter);
    if (it != FilterToJobResources_.end()) {
        return it->second;
    }

    TJobResources result;
    for (const auto& execNode : *ExecNodes_) {
        if (execNode->CanSchedule(filter)) {
            result += execNode->GetResourceLimits();
        }
    }

    FilterToJobResources_.insert({filter, result});

    return result;
}

TJobResources TSchedulerStrategyHost::GetResourceUsage(const TSchedulingTagFilter& /*filter*/) const
{
    return {};
}

void TSchedulerStrategyHost::Disconnect(const TError& /*error*/)
{
    YT_VERIFY(false);
}

TInstant TSchedulerStrategyHost::GetConnectionTime() const
{
    return TInstant();
}

TMemoryDistribution TSchedulerStrategyHost::GetExecNodeMemoryDistribution(
    const TSchedulingTagFilter& filter) const
{
    auto it = FilterToMemoryDistribution_.find(filter);
    if (it != FilterToMemoryDistribution_.end()) {
        return it->second;
    }

    TMemoryDistribution distribution;
    for (const auto& execNode : *ExecNodes_) {
        if (execNode->CanSchedule(filter)) {
            auto resourceLimits = execNode->GetResourceLimits();
            ++distribution[RoundUp(resourceLimits.GetMemory(), 1_GBs)];
        }
    }

    FilterToMemoryDistribution_.insert({filter, distribution});

    return distribution;
}

std::vector<NNodeTrackerClient::TNodeId> TSchedulerStrategyHost::GetExecNodeIds(
    const TSchedulingTagFilter& filter) const
{
    std::vector<NNodeTrackerClient::TNodeId> result;
    for (const auto& execNode : *ExecNodes_) {
        if (execNode->CanSchedule(filter)) {
            result.push_back(execNode->GetId());
        }
    }
    return result;
}

TString TSchedulerStrategyHost::GetExecNodeAddress(NNodeTrackerClient::TNodeId nodeId) const
{
    for (const auto& execNode : *ExecNodes_) {
        if (execNode->GetId() == nodeId) {
            return execNode->GetDefaultAddress();
        }
    }

    YT_ABORT();
}

TRefCountedExecNodeDescriptorMapPtr TSchedulerStrategyHost::CalculateExecNodeDescriptors(
    const TSchedulingTagFilter& filter) const
{
    auto result = New<TRefCountedExecNodeDescriptorMap>();

    for (const auto& execNode : *ExecNodes_) {
        if (execNode->CanSchedule(filter)) {
            YT_VERIFY(result->emplace(execNode->GetId(), execNode->BuildExecDescriptor()).second);
        }
    }

    return result;
}

void TSchedulerStrategyHost::UpdateNodesOnChangedTrees(
    const THashMap<TString, NScheduler::TSchedulingTagFilter>& /*treeIdToFilter*/)
{
    // Nothing to do.
}

std::optional<int> TSchedulerStrategyHost::FindMediumIndexByName(const TString& /*mediumName*/) const
{
    return {};
}

TString TSchedulerStrategyHost::FormatResources(const TJobResourcesWithQuota& resources) const
{
    return NScheduler::FormatResources(resources);
}

TString TSchedulerStrategyHost::FormatResourceUsage(
    const TJobResources& usage,
    const TJobResources& limits,
    const NNodeTrackerClient::NProto::TDiskResources& diskResources) const
{
    return NScheduler::FormatResourceUsage(usage, limits, diskResources, MediumDirectory_);
}

void TSchedulerStrategyHost::SerializeResources(const TJobResourcesWithQuota& resources, NYson::IYsonConsumer* consumer) const
{
    return NScheduler::SerializeJobResourcesWithQuota(resources, MediumDirectory_, consumer);
}
    
void TSchedulerStrategyHost::SerializeDiskQuota(const TDiskQuota& diskQuota, NYson::IYsonConsumer* consumer) const
{
    return NScheduler::SerializeDiskQuota(diskQuota, MediumDirectory_, consumer);
}

void TSchedulerStrategyHost::ValidatePoolPermission(
    const NYPath::TYPath& /*path*/,
    const TString& /*user*/,
    NYTree::EPermission /*permission*/) const
{ }

void TSchedulerStrategyHost::MarkOperationAsRunningInStrategy(TOperationId /*operationId*/)
{
    // Nothing to do.
}

void TSchedulerStrategyHost::AbortOperation(TOperationId /*operationId*/, const TError& /*error*/)
{
    YT_ABORT();
}

void TSchedulerStrategyHost::FlushOperationNode(TOperationId /*operationId*/)
{
    YT_ABORT();
}

void TSchedulerStrategyHost::PreemptJob(const TJobPtr& job, TDuration /*interruptTimeout*/)
{
    YT_VERIFY(job->GetNode()->Jobs().erase(job) == 1);
    job->SetState(NJobTrackerClient::EJobState::Aborted);
}

NYson::IYsonConsumer* TSchedulerStrategyHost::GetEventLogConsumer()
{
    YT_VERIFY(RemoteEventLogWriter_ || LocalEventLogWriter_);
    if (RemoteEventLogConsumer_) {
        return RemoteEventLogConsumer_.get();
    } else {
        return &LocalEventLogWriter_.value();
    }
}

const NLogging::TLogger* TSchedulerStrategyHost::GetEventLogger()
{
    return nullptr;
}

void TSchedulerStrategyHost::SetSchedulerAlert(ESchedulerAlertType alertType, const TError& alert)
{
    if (!alert.IsOK()) {
        YT_LOG_WARNING(alert, "Setting scheduler alert (AlertType: %lv)", alertType);
    }
}

TFuture<void> TSchedulerStrategyHost::SetOperationAlert(
    TOperationId /*operationId*/,
    EOperationAlertType /*alertType*/,
    const TError& /*alert*/,
    std::optional<TDuration> /*timeout*/)
{
    return VoidFuture;
}

void TSchedulerStrategyHost::LogResourceMetering(
    const TMeteringKey& /*key*/,
    const TMeteringStatistics& /*statistics*/,
    const THashMap<TString, TString>& /*otherTags*/,
    TInstant /*connectionTime*/,
    TInstant /*previousLogTime*/,
    TInstant /*currentTime*/)
{
    // Skip!
}

int TSchedulerStrategyHost::GetDefaultAbcId() const
{
    return -1;
}

void TSchedulerStrategyHost::InvokeStoringStrategyState(TPersistentStrategyStatePtr /*persistentStrategyState*/)
{ }

TFuture<void> TSchedulerStrategyHost::UpdateLastMeteringLogTime(TInstant /*time*/)
{
    return VoidFuture;
}

void TSchedulerStrategyHost::CloseEventLogger()
{
    if (RemoteEventLogWriter_) {
        WaitFor(RemoteEventLogWriter_->Close())
            .ThrowOnError();
    }
}

const THashMap<TString, TString>& TSchedulerStrategyHost::GetUserDefaultParentPoolMap() const
{
    static THashMap<TString, TString> stub;
    return stub;
}

} // namespace NYT::NSchedulerSimulator
