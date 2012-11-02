#pragma once

#include "public.h"
#include "operation_controller.h"
#include "chunk_pool.h"
#include "chunk_list_pool.h"
#include "private.h"

#include <ytlib/misc/thread_affinity.h>
#include <ytlib/misc/nullable.h>
#include <ytlib/misc/id_generator.h>

#include <ytlib/logging/tagged_logger.h>

#include <ytlib/actions/async_pipeline.h>
#include <ytlib/actions/cancelable_context.h>

#include <ytlib/table_client/table_ypath_proxy.h>

#include <ytlib/file_client/file_ypath_proxy.h>

#include <ytlib/cypress_client/public.h>
#include <ytlib/ytree/ypath_client.h>

#include <server/chunk_server/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////

class TOperationControllerBase
    : public IOperationController
{
public:
    TOperationControllerBase(
        TSchedulerConfigPtr config,
        IOperationHost* host,
        TOperation* operation);

    virtual void Initialize() override;
    virtual TFuture<void> Prepare() override;
    virtual TFuture<void> Revive() override;
    virtual TFuture<void> Commit() override;

    virtual void OnJobRunning(TJobPtr job, const NProto::TJobStatus& status) override;
    virtual void OnJobCompleted(TJobPtr job) override;
    virtual void OnJobFailed(TJobPtr job) override;
    virtual void OnJobAborted(TJobPtr job) override;

    virtual void OnNodeOffline(TExecNodePtr node) override;

    virtual void Abort() override;

    virtual TJobPtr ScheduleJob(
        ISchedulingContext* context,
        bool isStarving) override;

    virtual TCancelableContextPtr GetCancelableContext() override;
    virtual IInvokerPtr GetCancelableControlInvoker() override;
    virtual IInvokerPtr GetCancelableBackgroundInvoker() override;

    virtual int GetPendingJobCount() override;
    virtual NProto::TNodeResources GetUsedResources() override;
    virtual NProto::TNodeResources GetNeededResources() override;

    virtual void BuildProgressYson(NYson::IYsonConsumer* consumer) override;
    virtual void BuildResultYson(NYson::IYsonConsumer* consumer) override;

private:
    typedef TOperationControllerBase TThis;

protected:
    TSchedulerConfigPtr Config;
    IOperationHost* Host;
    TOperation* Operation;

    NObjectClient::TObjectServiceProxy ObjectProxy;
    mutable NLog::TTaggedLogger Logger;

    TCancelableContextPtr CancelableContext;
    IInvokerPtr CancelableControlInvoker;
    IInvokerPtr CancelableBackgroundInvoker;

    // Remains True as long as the operation is not finished.
    bool Active;

    // Remains True as long as the operation can schedule new jobs.
    bool Running;

    // Totals.
    int TotalInputChunkCount;
    i64 TotalInputDataSize;
    i64 TotalInputRowCount;
    i64 TotalInputValueCount;

    // Job counters.
    TProgressCounter JobCounter;

    // Increments each time a new job is scheduled.
    TIdGenerator<int> JobIndexGenerator;

    // Total resources used by all running jobs.
    NProto::TNodeResources UsedResources;

    // The transaction for reading input tables (nested inside scheduler transaction).
    // These tables are locked with Snapshot mode.
    NTransactionClient::ITransactionPtr InputTransaction;

    // The transaction for writing output tables (nested inside scheduler transaction).
    // These tables are locked with Shared mode.
    NTransactionClient::ITransactionPtr OutputTransaction;

    struct TTableBase
    {
        NYPath::TRichYPath Path;
        NObjectClient::TObjectId ObjectId;
    };

    // Input tables.
    struct TInputTable
        : public TTableBase
    {
        TInputTable()
            : NegateFetch(false)
        { }

        NTableClient::TTableYPathProxy::TRspFetchPtr FetchResponse;
        bool NegateFetch;
        TNullable< std::vector<Stroka> > KeyColumns;
    };

    std::vector<TInputTable> InputTables;

    // Output tables.
    struct TOutputTable
        : public TTableBase
    {
        TOutputTable()
            : Clear(false)
            , Overwrite(false)
            , LockMode(NCypressClient::ELockMode::Shared)
            , ReplicationFactor(0)
        { }

        bool Clear;
        bool Overwrite;
        NCypressClient::ELockMode LockMode;
        TNullable< std::vector<Stroka> > KeyColumns;
        NYTree::TYsonString Channels;
        int ReplicationFactor;

        // Chunk list for appending the output.
        NChunkClient::TChunkListId OutputChunkListId;

        //! Chunk trees comprising the output (the order matters).
        //! Keys are used when the output is sorted (e.g. in sort operations).
        //! Trees are sorted w.r.t. key and appended to #OutputChunkListId.
        std::multimap<int, NChunkServer::TChunkTreeId> OutputChunkTreeIds;
    };

    std::vector<TOutputTable> OutputTables;

    // Files.
    struct TUserFile
    {
        NYPath::TRichYPath Path;
        NFileClient::TFileYPathProxy::TRspFetchFilePtr FetchResponse;
    };

    std::vector<TUserFile> Files;

    // Forward declarations.

    class TTask;
    typedef TIntrusivePtr<TTask> TTaskPtr;

    struct TJoblet;
    typedef TIntrusivePtr<TJoblet> TJobletPtr;

    struct TJoblet
        : public TIntrinsicRefCounted
    {
        explicit TJoblet(TTaskPtr task, int jobIndex)
            : Task(task)
            , JobIndex(jobIndex)
            , StartRowIndex(-1)
            , OutputCookie(IChunkPoolOutput::NullCookie)
        { }

        TTaskPtr Task;
        int JobIndex;
        i64 StartRowIndex;

        TJobPtr Job;
        TChunkStripeListPtr InputStripeList;
        IChunkPoolOutput::TCookie OutputCookie;
        std::vector<NChunkClient::TChunkListId> ChunkListIds;
    };

    yhash_map<TJobPtr, TJobletPtr> JobsInProgress;

    // The set of all input chunks. Used in #OnChunkFailed.
    yhash_set<NChunkClient::TChunkId> InputChunkIds;

    // Tasks management.

    class TTask
        : public TRefCounted
    {
    public:
        explicit TTask(TOperationControllerBase* controller);

        virtual Stroka GetId() const = 0;
        virtual int GetPriority() const;

        virtual int GetPendingJobCount() const;
        int GetPendingJobCountDelta();

        virtual NProto::TNodeResources GetTotalNeededResources() const;
        NProto::TNodeResources GetTotalNeededResourcesDelta();
        
        virtual int GetChunkListCountPerJob() const = 0;
        
        virtual TDuration GetLocalityTimeout() const = 0;
        virtual i64 GetLocality(const Stroka& address) const;
        virtual bool IsStrictlyLocal() const;

        virtual NProto::TNodeResources GetMinNeededResources() const = 0;
        virtual NProto::TNodeResources GetAvgNeededResources() const;
        virtual NProto::TNodeResources GetNeededResources(TJobletPtr joblet) const;

        DEFINE_BYVAL_RW_PROPERTY(TNullable<TInstant>, DelayedTime);

        void AddInput(TChunkStripePtr stripe);
        void AddInput(const std::vector<TChunkStripePtr>& stripes);
        void FinishInput();

        TJobPtr ScheduleJob(ISchedulingContext* context);

        virtual void OnJobCompleted(TJobletPtr joblet);
        virtual void OnJobFailed(TJobletPtr joblet);
        virtual void OnJobAborted(TJobletPtr joblet);

        virtual void OnTaskCompleted();

        bool IsPending() const;
        bool IsCompleted() const;

        i64 GetTotalDataSize() const;
        i64 GetCompletedDataSize() const;
        i64 GetPendingDataSize() const;

    private:
        TOperationControllerBase* Controller;
        int CachedPendingJobCount;
        NProto::TNodeResources CachedTotalNeededResources;

    protected:
        NLog::TTaggedLogger& Logger;

        virtual IChunkPoolInput* GetChunkPoolInput() const = 0;
        virtual IChunkPoolOutput* GetChunkPoolOutput() const = 0;

        virtual void BuildJobSpec(
            TJobletPtr joblet,
            NProto::TJobSpec* jobSpec) = 0;

        virtual void OnJobStarted(TJobletPtr joblet);

        void AddPendingHint();
        virtual void AddInputLocalityHint(TChunkStripePtr stripe);

        static void AddSequentialInputSpec(
            NScheduler::NProto::TJobSpec* jobSpec, 
            TJobletPtr joblet,
            bool enableTableIndex = false);
        static void AddParallelInputSpec(
            NScheduler::NProto::TJobSpec* jobSpec, 
            TJobletPtr joblet,
            bool enableTableIndex = false);
        
        void AddOutputSpecs(NScheduler::NProto::TJobSpec* jobSpec, TJobletPtr joblet);
        void AddIntermediateOutputSpec(NScheduler::NProto::TJobSpec* jobSpec, TJobletPtr joblet);

    private:
        void ReleaseFailedJobResources(TJobletPtr joblet);

        static void AddInputChunks(
            NScheduler::NProto::TTableInputSpec* inputSpec,
            TChunkStripePtr stripe,
            TNullable<int> partitionTag,
            bool enableTableIndex);

        static void UpdateInputSpecTotals(
            NScheduler::NProto::TJobSpec* jobSpec,
            TJobletPtr joblet);
    };

    struct TPendingTaskInfo
    {
        yhash_set<TTaskPtr> GlobalTasks;
        yhash_map<Stroka, yhash_set<TTaskPtr>> AddressToLocalTasks;
    };

    static const int MaxTaskPriority = 2;
    std::vector<TPendingTaskInfo> PendingTaskInfos;

    int CachedPendingJobCount;
    NProto::TNodeResources CachedNeededResources;

    void OnTaskUpdated(TTaskPtr task);

    void DoAddTaskLocalityHint(TTaskPtr task, const Stroka& address);
    void AddTaskLocalityHint(TTaskPtr task, const Stroka& address);
    void AddTaskLocalityHint(TTaskPtr task, TChunkStripePtr stripe);
    void AddTaskPendingHint(TTaskPtr task);
    TPendingTaskInfo* GetPendingTaskInfo(TTaskPtr task);

    bool HasEnoughResources(TExecNodePtr node);
    bool HasEnoughResources(TTaskPtr task, TExecNodePtr node);

    TJobPtr DoScheduleJob(
        ISchedulingContext* context,
        bool isStarving);
    void OnJobStarted(TJobPtr job);

    DECLARE_THREAD_AFFINITY_SLOT(ControlThread);
    DECLARE_THREAD_AFFINITY_SLOT(BackgroundThread);

    // Jobs in progress management.
    void RegisterJobInProgress(TJobletPtr joblet);
    TJobletPtr GetJobInProgress(TJobPtr job);
    void RemoveJobInProgress(TJobPtr job);

    // Here comes the preparation pipeline.

    // Round 1:
    // - Start input transaction.
    // - Start output transaction.

    NObjectClient::TObjectServiceProxy::TInvExecuteBatch StartIOTransactions();

    void OnIOTransactionsStarted(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

    // Round 2:
    // - Get input table ids
    // - Get output table ids
    NObjectClient::TObjectServiceProxy::TInvExecuteBatch GetObjectIds();

    void OnObjectIdsReceived(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

    // Round 3:
    // - Fetch input tables.
    // - Lock input tables.
    // - Lock output tables.
    // - Fetch files.
    // - Get output tables channels.
    // - Get output chunk lists.
    // - (Custom)

    NObjectClient::TObjectServiceProxy::TInvExecuteBatch RequestInputs();
    void OnInputsReceived(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

    //! Extensibility point for requesting additional info from master.
    virtual void RequestCustomInputs(NObjectClient::TObjectServiceProxy::TReqExecuteBatchPtr batchReq);

    //! Extensibility point for handling additional info from master.
    virtual void OnCustomInputsRecieved(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

    // Round 4.
    // - (Custom)
    virtual TAsyncPipeline<void>::TPtr CustomizePreparationPipeline(TAsyncPipeline<void>::TPtr pipeline);

    // Round 5.
    // - Collect totals.
    // - Check for empty inputs.
    // - Init chunk list pool.
    TFuture<void> CompletePreparation();
    void OnPreparationCompleted();

    // Here comes the completion pipeline.

    // Round 1.
    // - Attach chunk trees.
    // - Commit input transaction.
    // - Commit output transaction.
    // - Commit scheduler transaction.

    NObjectClient::TObjectServiceProxy::TInvExecuteBatch CommitOutputs();
    void OnOutputsCommitted(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

    virtual void DoInitialize();
    virtual void LogProgress() = 0;

    //! Called to extract input table paths from the spec.
    virtual std::vector<NYPath::TRichYPath> GetInputTablePaths() const = 0;
    
    //! Called to extract output table paths from the spec.
    virtual std::vector<NYPath::TRichYPath> GetOutputTablePaths() const = 0;
    
    //! Called to extract file paths from the spec.
    virtual std::vector<NYPath::TRichYPath> GetFilePaths() const;


    //! Called when a job is unable to read a chunk.
    void OnChunkFailed(const NChunkClient::TChunkId& chunkId);

    //! Called when a job is unable to read an intermediate chunk
    //! (i.e. that is not a part of the input).
    /*!
     *  The default implementation fails the operation immediately.
     *  Those operations providing some fault tolerance for intermediate chunks
     *  must override this method.
     */
    virtual void OnIntermediateChunkFailed(const NChunkClient::TChunkId& chunkId);

    //! Called when a job is unable to read an input chunk.
    /*!
     *  The operation fails immediately.
     */
    void OnInputChunkFailed(const NChunkClient::TChunkId& chunkId);

    
    // Abort is not a pipeline really :)

    void AbortTransactions();


    virtual void OnOperationCompleted();
    virtual void OnOperationFailed(const TError& error);


    // Unsorted helpers.

    std::vector<Stroka> CheckInputTablesSorted(
        const TNullable< std::vector<Stroka> >& keyColumns);
    static bool CheckKeyColumnsCompatible(
        const std::vector<Stroka>& fullColumns,
        const std::vector<Stroka>& prefixColumns);

    void RegisterOutputChunkTree(
        const NChunkServer::TChunkTreeId& chunkTreeId,
        int key,
        int tableIndex);
    void RegisterOutputChunkTrees(
        TJobletPtr joblet,
        int key);

    static TChunkStripePtr BuildIntermediateChunkStripe(
        google::protobuf::RepeatedPtrField<NTableClient::NProto::TInputChunk>* inputChunks);

    bool HasEnoughChunkLists(int requestedCount);
    NChunkClient::TChunkListId ExtractChunkList();

    void ReleaseChunkList(const NChunkClient::TChunkListId& id);
    void ReleaseChunkLists(const std::vector<NChunkClient::TChunkListId>& ids);

    //! Returns the list of all input chunks collected from all input tables.
    std::vector<NTableClient::TRefCountedInputChunkPtr> CollectInputChunks();

    //! Converts a list of input chunks into a list of chunk stripes for further
    //! processing. Each stripe receives exactly one chunk (as suitable for most
    //! jobs except merge). Tries to slice chunks into smaller parts if
    //! sees necessary based on #jobCount and #jobSliceWeight.
    std::vector<TChunkStripePtr> SliceInputChunks(
        TNullable<int> jobCount,
        i64 jobSliceWeight);

    int SuggestJobCount(
        i64 totalDataSize,
        i64 minDataSizePerJob,
        i64 maxDataSizePerJob,
        TNullable<int> configJobCount,
        int chunkCount);

    void InitUserJobSpec(
        NScheduler::NProto::TUserJobSpec* proto,
        TUserJobSpecPtr config,
        const std::vector<TUserFile>& files);

    static void AddUserJobEnvironment(
        NScheduler::NProto::TUserJobSpec* proto, 
        TJobletPtr joblet);

    static void InitIntermediateInputConfig(TJobIOConfigPtr config);

    static void InitIntermediateOutputConfig(TJobIOConfigPtr config);
    void InitFinalOutputConfig(TJobIOConfigPtr config);

private:
    TChunkListPoolPtr ChunkListPool;

    void OnChunkListsReleased(NObjectClient::TObjectServiceProxy::TRspExecuteBatchPtr batchRsp);

};

////////////////////////////////////////////////////////////////////////////////

namespace {

template <class TSpec>
TIntrusivePtr<TSpec> ParseOperationSpec(TOperation* operation, NYTree::INodePtr defaultSpec)
{
    auto ysonSpec = NYTree::UpdateNode(defaultSpec, operation->GetSpec());
    auto spec = New<TSpec>();
    try {
        spec->Load(ysonSpec);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing operation spec") << ex;
    }
    return spec;
}

} // namespace

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
