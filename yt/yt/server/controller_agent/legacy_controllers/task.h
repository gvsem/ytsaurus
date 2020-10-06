#pragma once

#include "private.h"

#include "competitive_job_manager.h"
#include "data_flow_graph.h"

#include <yt/server/controller_agent/tentative_tree_eligibility.h>

#include <yt/server/lib/legacy_chunk_pools/chunk_stripe_key.h>
#include <yt/server/lib/legacy_chunk_pools/chunk_pool.h>
#include <yt/server/lib/legacy_chunk_pools/input_chunk_mapping.h>

#include <yt/server/lib/controller_agent/legacy_progress_counter.h>
#include <yt/server/lib/controller_agent/serialize.h>

#include <yt/ytlib/scheduler/job_resources.h>
#include <yt/ytlib/scheduler/public.h>

#include <yt/ytlib/table_client/helpers.h>

#include <yt/core/misc/digest.h>
#include <yt/core/misc/histogram.h>

namespace NYT::NControllerAgent::NLegacyControllers {

////////////////////////////////////////////////////////////////////////////////

class TTask
    : public TRefCounted
    , public IPersistent
{
public:
    DEFINE_BYVAL_RW_PROPERTY(std::optional<TInstant>, DelayedTime);
    DEFINE_BYVAL_RW_PROPERTY(TDataFlowGraph::TVertexDescriptor, InputVertex, TDataFlowGraph::TVertexDescriptor());

public:
    //! For persistence only.
    TTask();
    TTask(ITaskHostPtr taskHost, std::vector<TEdgeDescriptor> edgeDescriptors);
    explicit TTask(ITaskHostPtr taskHost);

    //! This method is called on task object creation at clean creation but not at revival.
    //! It may be used when calling virtual method is needed, but not allowed.
    virtual void Prepare();

    //! This method is called on task object creation (both at clean creation and at revival).
    //! It may be used when calling virtual method is needed, but not allowed.
    virtual void Initialize();

    //! Title of a data flow graph vertex that appears in a web interface and coincides with the job type
    //! for builtin tasks. For example, "SortedReduce" or "PartitionMap".
    virtual TDataFlowGraph::TVertexDescriptor GetVertexDescriptor() const;
    //! Human-readable title of a particular task that appears in logging. For builtin tasks it coincides
    //! with the vertex descriptor and a partition index in brackets (if applicable).
    virtual TString GetTitle() const;

    //! Human-readable name of a particular task that appears in archive. Supported for vanilla tasks only for now.
    virtual TString GetName() const;

    virtual TTaskGroupPtr GetGroup() const = 0;

    virtual int GetPendingJobCount() const;
    int GetPendingJobCountDelta();

    virtual int GetTotalJobCount() const;
    int GetTotalJobCountDelta();

    const TLegacyProgressCounterPtr& GetJobCounter() const;

    virtual TJobResources GetTotalNeededResources() const;
    TJobResources GetTotalNeededResourcesDelta();

    bool IsStderrTableEnabled() const;

    bool IsCoreTableEnabled() const;

    virtual TDuration GetLocalityTimeout() const;
    virtual i64 GetLocality(NNodeTrackerClient::TNodeId nodeId) const;
    virtual bool HasInputLocality() const;

    NScheduler::TJobResourcesWithQuota GetMinNeededResources() const;

    void ResetCachedMinNeededResources();

    void AddInput(NLegacyChunkPools::TChunkStripePtr stripe);
    void AddInput(const std::vector<NLegacyChunkPools::TChunkStripePtr>& stripes);

    // NB: This works well until there is no more than one input data flow vertex for any task.
    void FinishInput(TDataFlowGraph::TVertexDescriptor inputVertex);
    virtual void FinishInput();

    void CheckCompleted();
    void ForceComplete();

    virtual bool ValidateChunkCount(int chunkCount);

    void ScheduleJob(
        ISchedulingContext* context,
        const NScheduler::TJobResourcesWithQuota& jobLimits,
        const TString& treeId,
        bool treeIsTentative,
        NScheduler::TControllerScheduleJobResult* scheduleJobResult);

    bool TryRegisterSpeculativeJob(const TJobletPtr& joblet);
    std::optional<EAbortReason> ShouldAbortJob(const TJobletPtr& joblet);

    virtual TJobFinishedResult OnJobCompleted(TJobletPtr joblet, TCompletedJobSummary& jobSummary);
    virtual TJobFinishedResult OnJobFailed(TJobletPtr joblet, const TFailedJobSummary& jobSummary);
    virtual TJobFinishedResult OnJobAborted(TJobletPtr joblet, const TAbortedJobSummary& jobSummary);

    virtual void OnJobLost(TCompletedJobPtr completedJob);

    virtual void OnStripeRegistrationFailed(
        TError error,
        NLegacyChunkPools::IChunkPoolInput::TCookie cookie,
        const NLegacyChunkPools::TChunkStripePtr& stripe,
        const TEdgeDescriptor& edgeDescriptor);

    // First checks against a given node, then against all nodes if needed.
    void CheckResourceDemandSanity(
        const NScheduler::TJobResourcesWithQuota& nodeResourceLimits,
        const NScheduler::TJobResourcesWithQuota& neededResources);

    void DoCheckResourceDemandSanity(const NScheduler::TJobResourcesWithQuota& neededResources);

    virtual bool IsCompleted() const;

    virtual bool IsActive() const;

    i64 GetTotalDataWeight() const;
    i64 GetCompletedDataWeight() const;
    i64 GetPendingDataWeight() const;

    i64 GetInputDataSliceCount() const;

    std::vector<std::optional<i64>> GetMaximumUsedTmpfsSizes() const;

    virtual void Persist(const TPersistenceContext& context) override;

    virtual NScheduler::TUserJobSpecPtr GetUserJobSpec() const;
    bool HasUserJob() const;

    // TODO(max42): eliminate necessity for this method (YT-10528).
    virtual bool IsSimpleTask() const;

    ITaskHost* GetTaskHost();
    void AddLocalityHint(NNodeTrackerClient::TNodeId nodeId);
    void AddPendingHint();

    IDigest* GetUserJobMemoryDigest() const;
    IDigest* GetJobProxyMemoryDigest() const;

    virtual void SetupCallbacks();

    virtual NScheduler::TExtendedJobResources GetNeededResources(const TJobletPtr& joblet) const = 0;

    virtual NLegacyChunkPools::IChunkPoolInputPtr GetChunkPoolInput() const = 0;
    virtual NLegacyChunkPools::IChunkPoolOutputPtr GetChunkPoolOutput() const = 0;

    virtual EJobType GetJobType() const = 0;

    //! Return a chunk mapping that is used to substitute input chunks when job spec is built.
    //! Base implementation returns task's own mapping.
    virtual NLegacyChunkPools::TInputChunkMappingPtr GetChunkMapping() const;

    std::vector<TString> FindAndBanSlowTentativeTrees();

    void LogTentativeTreeStatistics() const;

    TSharedRef BuildJobSpecProto(TJobletPtr joblet);

    virtual bool IsJobInterruptible() const;

    void BuildTaskYson(NYTree::TFluentMap fluent) const;

    virtual void PropagatePartitions(
        const std::vector<TEdgeDescriptor>& edgeDescriptors,
        const NLegacyChunkPools::TChunkStripeListPtr& inputStripeList,
        std::vector<NLegacyChunkPools::TChunkStripePtr>* outputStripes);

protected:
    NLogging::TLogger Logger;

    //! Raw pointer here avoids cyclic reference; task cannot live longer than its host.
    ITaskHost* TaskHost_;

    //! Outgoing edges in data flow graph.
    std::vector<TEdgeDescriptor> EdgeDescriptors_;

    //! Increments each time a new job in this task is scheduled.
    TIdGenerator TaskJobIndexGenerator_;

    TTentativeTreeEligibility TentativeTreeEligibility_;

    mutable std::unique_ptr<IDigest> JobProxyMemoryDigest_;
    mutable std::unique_ptr<IDigest> UserJobMemoryDigest_;

    virtual std::optional<EScheduleJobFailReason> GetScheduleFailReason(ISchedulingContext* context);

    virtual void OnTaskCompleted();

    virtual void OnJobStarted(TJobletPtr joblet);

    //! True if task supports lost jobs.
    virtual bool CanLoseJobs() const;

    virtual void OnChunkTeleported(NChunkClient::TInputChunkPtr chunk, std::any tag);

    void ReinstallJob(TJobletPtr joblet, std::function<void()> releaseOutputCookie);

    void ReleaseJobletResources(TJobletPtr joblet, bool waitForSnapshot);

    std::unique_ptr<NNodeTrackerClient::TNodeDirectoryBuilder> MakeNodeDirectoryBuilder(
        NScheduler::NProto::TSchedulerJobSpecExt* schedulerJobSpec);
    void AddSequentialInputSpec(
        NJobTrackerClient::NProto::TJobSpec* jobSpec,
        TJobletPtr joblet);
    void AddParallelInputSpec(
        NJobTrackerClient::NProto::TJobSpec* jobSpec,
        TJobletPtr joblet);
    void AddChunksToInputSpec(
        NNodeTrackerClient::TNodeDirectoryBuilder* directoryBuilder,
        NScheduler::NProto::TTableInputSpec* inputSpec,
        NLegacyChunkPools::TChunkStripePtr stripe);

    void AddOutputTableSpecs(NJobTrackerClient::NProto::TJobSpec* jobSpec, TJobletPtr joblet);

    static void UpdateInputSpecTotals(
        NJobTrackerClient::NProto::TJobSpec* jobSpec,
        TJobletPtr joblet);

    // Send stripe to the next chunk pool.
    void RegisterStripe(
        NLegacyChunkPools::TChunkStripePtr chunkStripe,
        const TEdgeDescriptor& edgeDescriptor,
        TJobletPtr joblet,
        NLegacyChunkPools::TChunkStripeKey key = NLegacyChunkPools::TChunkStripeKey());

    static std::vector<NLegacyChunkPools::TChunkStripePtr> BuildChunkStripes(
        google::protobuf::RepeatedPtrField<NChunkClient::NProto::TChunkSpec>* chunkSpecs,
        int tableCount);

    static NLegacyChunkPools::TChunkStripePtr BuildIntermediateChunkStripe(
        google::protobuf::RepeatedPtrField<NChunkClient::NProto::TChunkSpec>* chunkSpecs);

    std::vector<NLegacyChunkPools::TChunkStripePtr> BuildOutputChunkStripes(
        NScheduler::NProto::TSchedulerJobResultExt* schedulerJobResultExt,
        const std::vector<NChunkClient::TChunkTreeId>& chunkTreeIds,
        google::protobuf::RepeatedPtrField<NScheduler::NProto::TOutputResult> boundaryKeys);

    void AddFootprintAndUserJobResources(NScheduler::TExtendedJobResources& jobResources) const;

    //! This method processes `chunkListIds`, forming the chunk stripes (maybe with boundary
    //! keys taken from `jobResult` if they are present) and sends them to the destination pools
    //! depending on the table index.
    //!
    //! If destination pool requires the recovery info, `joblet` should be non-null since it is used
    //! in the recovery info, otherwise it is not used.
    //!
    //! This method steals output chunk specs for `jobResult`.
    void RegisterOutput(
        NJobTrackerClient::NProto::TJobResult* jobResult,
        const std::vector<NChunkClient::TChunkListId>& chunkListIds,
        TJobletPtr joblet,
        const NLegacyChunkPools::TChunkStripeKey& key = NLegacyChunkPools::TChunkStripeKey());

    //! A convenience method for calling task->Finish() and
    //! task->SetInputVertex(this->GetJobType());
    void FinishTaskInput(const TTaskPtr& task);

    virtual NScheduler::TExtendedJobResources GetMinNeededResourcesHeavy() const = 0;
    virtual void BuildJobSpec(TJobletPtr joblet, NJobTrackerClient::NProto::TJobSpec* jobSpec) = 0;

    virtual void SetEdgeDescriptors(TJobletPtr joblet) const;

    virtual bool IsInputDataWeightHistogramSupported() const;

private:
    DECLARE_DYNAMIC_PHOENIX_TYPE(TTask, 0x81ab3cd4);

    int CachedPendingJobCount_;
    int CachedTotalJobCount_;

    std::vector<std::optional<i64>> MaximumUsedTmpfsSizes_;

    TJobResources CachedTotalNeededResources_;
    mutable std::optional<NScheduler::TExtendedJobResources> CachedMinNeededResources_;

    bool CompletedFired_ = false;

    using TCookieAndPool = std::pair<NLegacyChunkPools::IChunkPoolInput::TCookie, NLegacyChunkPools::IChunkPoolInputPtr>;

    //! For each lost job currently being replayed and destination pool, maps output cookie to corresponding input cookie.
    std::map<TCookieAndPool, NLegacyChunkPools::IChunkPoolInput::TCookie> LostJobCookieMap;

    NLegacyChunkPools::TInputChunkMappingPtr InputChunkMapping_;

    TCompetitiveJobManager CompetitiveJobManager_;

    //! Time of first job scheduling.
    std::optional<TInstant> StartTime_;

    //! Time of task completion.
    std::optional<TInstant> CompletionTime_;

    //! Caches results of SerializeToWireProto serializations.
    // NB: This field is transient intentionally.
    THashMap<NTableClient::TTableSchemaPtr, TString> TableSchemaToProtobufTableSchema_;

    std::unique_ptr<IHistogram> EstimatedInputDataWeightHistogram_;
    std::unique_ptr<IHistogram> InputDataWeightHistogram_;

    NScheduler::TJobResources ApplyMemoryReserve(const NScheduler::TExtendedJobResources& jobResources) const;

    void UpdateMaximumUsedTmpfsSizes(const TStatistics& statistics);

    void AbortJobViaScheduler(TJobId jobId, EAbortReason reason);

    void OnSpeculativeJobScheduled(const TJobletPtr& joblet);

    double GetJobProxyMemoryReserveFactor() const;
    double GetUserJobMemoryReserveFactor() const;

    TString GetOrCacheSerializedSchema(const NTableClient::TTableSchemaPtr& schema);
};

DEFINE_REFCOUNTED_TYPE(TTask)

////////////////////////////////////////////////////////////////////////////////

//! Groups provide means:
//! - to prioritize tasks
//! - to skip a vast number of tasks whose resource requirements cannot be met
struct TTaskGroup
    : public TRefCounted
{
    //! No task from this group is considered for scheduling unless this requirement is met.
    NScheduler::TJobResourcesWithQuota MinNeededResources;

    //! All non-local tasks.
    THashSet<TTaskPtr> NonLocalTasks;

    //! Non-local tasks that may possibly be ready (but a delayed check is still needed)
    //! keyed by min memory demand (as reported by TTask::GetMinNeededResources).
    std::multimap<i64, TTaskPtr> CandidateTasks;

    //! Non-local tasks keyed by deadline.
    std::multimap<TInstant, TTaskPtr> DelayedTasks;

    //! Local tasks keyed by node id.
    THashMap<NNodeTrackerClient::TNodeId, THashSet<TTaskPtr>> NodeIdToTasks;

    TTaskGroup();

    void Persist(const TPersistenceContext& context);
};

DEFINE_REFCOUNTED_TYPE(TTaskGroup)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NControllerAgent::NLegacyControllers
