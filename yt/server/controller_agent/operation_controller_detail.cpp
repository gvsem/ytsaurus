#include "operation_controller_detail.h"
#include "auto_merge_task.h"
#include "intermediate_chunk_scraper.h"
#include "job_info.h"
#include "job_helpers.h"
#include "counter_manager.h"
#include "task.h"
#include "operation.h"
#include "scheduling_context.h"
#include "config.h"

#include <yt/server/scheduler/helpers.h>
#include <yt/server/scheduler/job.h>

#include <yt/server/misc/job_table_schema.h>

#include <yt/server/chunk_pools/helpers.h>

#include <yt/ytlib/chunk_client/chunk_meta_extensions.h>
#include <yt/ytlib/chunk_client/chunk_scraper.h>
#include <yt/ytlib/chunk_client/chunk_teleporter.h>
#include <yt/ytlib/chunk_client/data_slice_descriptor.h>
#include <yt/ytlib/chunk_client/data_statistics.h>
#include <yt/ytlib/chunk_client/data_source.h>
#include <yt/ytlib/chunk_client/helpers.h>
#include <yt/ytlib/chunk_client/input_chunk_slice.h>
#include <yt/ytlib/chunk_client/input_data_slice.h>
#include <yt/ytlib/chunk_client/job_spec_extensions.h>

#include <yt/ytlib/cypress_client/rpc_helpers.h>

#include <yt/ytlib/core_dump/proto/core_info.pb.h>
#include <yt/ytlib/core_dump/helpers.h>

#include <yt/ytlib/event_log/event_log.h>

#include <yt/ytlib/node_tracker_client/node_directory_builder.h>

#include <yt/ytlib/object_client/helpers.h>

#include <yt/ytlib/query_client/column_evaluator.h>
#include <yt/ytlib/query_client/functions_cache.h>
#include <yt/ytlib/query_client/query.h>
#include <yt/ytlib/query_client/query_preparer.h>
#include <yt/ytlib/query_client/range_inferrer.h>

#include <yt/ytlib/scheduler/helpers.h>

#include <yt/ytlib/table_client/chunk_meta_extensions.h>
#include <yt/ytlib/table_client/columnar_statistics_fetcher.h>
#include <yt/ytlib/table_client/data_slice_fetcher.h>
#include <yt/ytlib/table_client/helpers.h>
#include <yt/ytlib/table_client/schema.h>
#include <yt/ytlib/table_client/table_consumer.h>
#include <yt/ytlib/table_client/row_buffer.h>

#include <yt/ytlib/transaction_client/helpers.h>

#include <yt/ytlib/api/transaction.h>
#include <yt/ytlib/api/native_connection.h>

#include <yt/core/concurrency/action_queue.h>
#include <yt/core/concurrency/throughput_throttler.h>

#include <yt/core/erasure/codec.h>

#include <yt/core/misc/fs.h>
#include <yt/core/misc/chunked_input_stream.h>
#include <yt/core/misc/collection_helpers.h>
#include <yt/core/misc/numeric_helpers.h>

#include <yt/core/profiling/timing.h>
#include <yt/core/profiling/profiler.h>

#include <yt/core/logging/log.h>

#include <yt/core/ytree/virtual.h>

#include <util/generic/vector.h>

#include <functional>

namespace NYT {
namespace NControllerAgent {

using namespace NChunkPools;
using namespace NCypressClient;
using namespace NTransactionClient;
using namespace NFileClient;
using namespace NChunkClient;
using namespace NObjectClient;
using namespace NYTree;
using namespace NYson;
using namespace NYPath;
using namespace NFormats;
using namespace NJobProxy;
using namespace NJobTrackerClient;
using namespace NNodeTrackerClient;
using namespace NScheduler::NProto;
using namespace NJobTrackerClient::NProto;
using namespace NCoreDump::NProto;
using namespace NConcurrency;
using namespace NApi;
using namespace NRpc;
using namespace NTableClient;
using namespace NQueryClient;
using namespace NProfiling;
using namespace NScheduler;
using namespace NEventLog;
using namespace NLogging;

using NNodeTrackerClient::TNodeId;
using NProfiling::CpuInstantToInstant;
using NProfiling::TCpuInstant;
using NTableClient::NProto::TBoundaryKeysExt;
using NTableClient::TTableReaderOptions;
using NScheduler::TExecNodeDescriptor;

using std::placeholders::_1;

////////////////////////////////////////////////////////////////////////////////

static class TJobHelper
{
public:
    TJobHelper()
    {
        for (auto state : TEnumTraits<EJobState>::GetDomainValues()) {
            for (auto type : TEnumTraits<EJobType>::GetDomainValues()) {
                StatisticsSuffixes_[state][type] = Format("/$/%lv/%lv", state, type);
            }
        }
    }

    const TString& GetStatisticsSuffix(EJobState state, EJobType type) const
    {
        return StatisticsSuffixes_[state][type];
    }

private:
    TEnumIndexedVector<TEnumIndexedVector<TString, EJobType>, EJobState> StatisticsSuffixes_;

} JobHelper;

////////////////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TStripeDescriptor::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Stripe);
    Persist(context, Cookie);
    Persist(context, Task);
}

////////////////////////////////////////////////////////////////////////////////

void TOperationControllerBase::TInputChunkDescriptor::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, InputStripes);
    Persist(context, InputChunks);
    Persist(context, State);
}

////////////////////////////////////////////////////////////////////////////////

TOperationControllerBase::TOperationControllerBase(
    TOperationSpecBasePtr spec,
    TControllerAgentConfigPtr config,
    TOperationOptionsPtr options,
    IOperationControllerHostPtr host,
    TOperation* operation)
    : Host(std::move(host))
    , Config(std::move(config))
    , OperationId(operation->GetId())
    , OperationType(operation->GetType())
    , StartTime(operation->GetStartTime())
    , AuthenticatedUser(operation->GetAuthenticatedUser())
    , SecureVault(operation->GetSecureVault())
    , Owners(operation->GetOwners())
    , UserTransactionId(operation->GetUserTransactionId())
    , Logger(TLogger(ControllerLogger)
        .AddTag("OperationId: %v", OperationId))
    , CoreNotes_({
        Format("OperationId: %v", OperationId)
    })
    , CancelableContext(New<TCancelableContext>())
    , Invoker(CreateMemoryTaggingInvoker(CreateSerializedInvoker(Host->GetControllerThreadPoolInvoker()), operation->GetMemoryTag()))
    , SuspendableInvoker(CreateSuspendableInvoker(Invoker))
    , CancelableInvoker(CancelableContext->CreateInvoker(SuspendableInvoker))
    , JobCounter(New<TProgressCounter>(0))
    , RowBuffer(New<TRowBuffer>(TRowBufferTag(), Config->ControllerRowBufferChunkSize))
    , MemoryTag_(operation->GetMemoryTag())
    , PoolTreeSchedulingTagFilters_(operation->PoolTreeSchedulingTagFilters())
    , Spec_(std::move(spec))
    , Options(std::move(options))
    , SuspiciousJobsYsonUpdater_(New<TPeriodicExecutor>(
        GetCancelableInvoker(),
        BIND(&TThis::UpdateSuspiciousJobsYson, MakeWeak(this)),
        Config->SuspiciousJobs->UpdatePeriod))
    , ScheduleJobStatistics_(New<TScheduleJobStatistics>())
    , CheckTimeLimitExecutor(New<TPeriodicExecutor>(
        GetCancelableInvoker(),
        BIND(&TThis::CheckTimeLimit, MakeWeak(this)),
        Config->OperationTimeLimitCheckPeriod))
    , ExecNodesCheckExecutor(New<TPeriodicExecutor>(
        GetCancelableInvoker(),
        BIND(&TThis::CheckAvailableExecNodes, MakeWeak(this)),
        Config->AvailableExecNodesCheckPeriod))
    , AnalyzeOperationProgressExecutor(New<TPeriodicExecutor>(
        GetCancelableInvoker(),
        BIND(&TThis::AnalyzeOperationProgress, MakeWeak(this)),
        Config->OperationProgressAnalysisPeriod))
    , MinNeededResourcesSanityCheckExecutor(New<TPeriodicExecutor>(
        GetCancelableInvoker(),
        BIND(&TThis::CheckMinNeededResourcesSanity, MakeWeak(this)),
        Config->ResourceDemandSanityCheckPeriod))
    , MaxAvailableExecNodeResourcesUpdateExecutor(New<TPeriodicExecutor>(
        GetCancelableInvoker(),
        BIND(&TThis::UpdateCachedMaxAvailableExecNodeResources, MakeWeak(this)),
        Config->MaxAvailableExecNodeResourcesUpdatePeriod))
    , EventLogConsumer_(Host->GetEventLogWriter()->CreateConsumer())
    , LogProgressBackoff(DurationToCpuDuration(Config->OperationLogProgressBackoff))
    , ProgressBuildExecutor_(New<TPeriodicExecutor>(
        GetCancelableInvoker(),
        BIND(&TThis::BuildAndSaveProgress, MakeWeak(this)),
        Config->OperationBuildProgressPeriod))
{
    // Attach user transaction if any. Don't ping it.
    TTransactionAttachOptions userAttachOptions;
    userAttachOptions.Ping = false;
    userAttachOptions.PingAncestors = false;
    UserTransaction = UserTransactionId
        ? Host->GetClient()->AttachTransaction(UserTransactionId, userAttachOptions)
        : nullptr;
}

void TOperationControllerBase::BuildMemoryUsageYson(TFluentAny fluent) const
{
    fluent
        .Value(GetMemoryUsage());
}

void TOperationControllerBase::BuildStateYson(TFluentAny fluent) const
{
    fluent
        .Value(State.load());
}

// Resource management.
TExtendedJobResources TOperationControllerBase::GetAutoMergeResources(
    const TChunkStripeStatisticsVector& statistics) const
{
    TExtendedJobResources result;
    result.SetUserSlots(1);
    result.SetCpu(1);
    // TODO(max42): this way to estimate memory of an auto-merge job is wrong as it considers each
    // auto-merge task writing to all output tables.
    result.SetJobProxyMemory(GetFinalIOMemorySize(Spec_->AutoMerge->JobIO, AggregateStatistics(statistics)));
    return result;
}

const TJobSpec& TOperationControllerBase::GetAutoMergeJobSpecTemplate(int tableIndex) const
{
    return AutoMergeJobSpecTemplates_[tableIndex];
}

void TOperationControllerBase::InitializeClients()
{
    TClientOptions options;
    options.User = AuthenticatedUser;
    Client = Host
        ->GetClient()
        ->GetNativeConnection()
        ->CreateNativeClient(options);
    InputClient = Client;
    OutputClient = Client;
}

TOperationControllerInitializationResult TOperationControllerBase::InitializeReviving(const TControllerTransactions& transactions)
{
    LOG_INFO("Initializing operation for revive");

    InitializeClients();

    auto attachTransaction = [&] (const TTransactionId& transactionId, bool ping) -> ITransactionPtr {
        if (!transactionId) {
            return nullptr;
        }

        try {
            return AttachTransaction(transactionId, ping);
        } catch (const std::exception& ex) {
            LOG_WARNING(ex, "Error attaching operation transaction (OperationId: %v, TransactionId: %v)",
                OperationId,
                transactionId);
            return nullptr;
        }
    };

    auto inputTransaction = attachTransaction(transactions.InputId, true);
    auto outputTransaction = attachTransaction(transactions.OutputId, true);
    auto debugTransaction = attachTransaction(transactions.DebugId, true);
    // NB: Async and completion transactions are never reused and thus are not pinged.
    auto asyncTransaction = attachTransaction(transactions.AsyncId, false);
    auto outputCompletionTransaction = attachTransaction(transactions.OutputCompletionId, false);
    auto debugCompletionTransaction = attachTransaction(transactions.DebugCompletionId, false);

    bool cleanStart = false;

    // Check transactions.
    {
        std::vector<std::pair<ITransactionPtr, TFuture<void>>> asyncCheckResults;

        auto checkTransaction = [&] (const ITransactionPtr& transaction) {
            if (cleanStart) {
                return;
            }

            if (!transaction) {
                cleanStart = true;
                LOG_INFO("Operation transaction is missing, will use clean start");
                return;
            }

            asyncCheckResults.emplace_back(transaction, transaction->Ping());
        };

        // NB: Async transaction is not checked.
        if (IsTransactionNeeded(ETransactionType::Input)) {
            checkTransaction(inputTransaction);
        }
        if (IsTransactionNeeded(ETransactionType::Output)) {
            checkTransaction(outputTransaction);
        }
        if (IsTransactionNeeded(ETransactionType::Debug)) {
            checkTransaction(debugTransaction);
        }

        for (const auto& pair : asyncCheckResults) {
            const auto& transaction = pair.first;
            const auto& asyncCheckResult = pair.second;
            auto error = WaitFor(asyncCheckResult);
            if (!error.IsOK()) {
                cleanStart = true;
                LOG_INFO(error,
                    "Error renewing operation transaction, will use clean start (TransactionId: %v)",
                    transaction->GetId());
            }
        }
    }

    // Downloading snapshot.
    if (!cleanStart) {
        auto snapshotOrError = WaitFor(Host->DownloadSnapshot());
        if (!snapshotOrError.IsOK()) {
            LOG_INFO(snapshotOrError, "Failed to download snapshot, will use clean start");
            cleanStart = true;
        } else {
            LOG_INFO("Snapshot successfully downloaded");
            Snapshot = snapshotOrError.Value();
        }
    }

    // Abort transactions if needed.
    {
        std::vector<TFuture<void>> asyncResults;

        auto scheduleAbort = [&] (const ITransactionPtr& transaction) {
            if (transaction) {
                // Transaction object may be in incorrect state, we need to abort using only transaction id.
                asyncResults.push_back(AttachTransaction(transaction->GetId())->Abort());
            }
        };

        scheduleAbort(asyncTransaction);
        scheduleAbort(outputCompletionTransaction);
        scheduleAbort(debugCompletionTransaction);

        if (cleanStart) {
            LOG_INFO("Aborting operation transactions");
            // NB: Don't touch user transaction.
            scheduleAbort(inputTransaction);
            scheduleAbort(outputTransaction);
            scheduleAbort(debugTransaction);
        } else {
            LOG_INFO("Reusing operation transactions");
            InputTransaction = inputTransaction;
            OutputTransaction = outputTransaction;
            DebugTransaction = debugTransaction;
            AsyncTransaction = WaitFor(StartTransaction(ETransactionType::Async, Client))
                .ValueOrThrow();
        }

        WaitFor(Combine(asyncResults))
            .ThrowOnError();
    }


    if (cleanStart) {
        if (Spec_->FailOnJobRestart) {
            THROW_ERROR_EXCEPTION("Cannot use clean restart when spec option fail_on_job_restart is set");
        }

        LOG_INFO("Using clean start instead of revive");

        Snapshot = TOperationSnapshot();
        Y_UNUSED(WaitFor(Host->RemoveSnapshot()));

        StartTransactions();
        InitializeStructures();

        SyncPrepare();
    }

    InitUnrecognizedSpec();

    WaitFor(Host->UpdateInitializedOperationNode())
        .ThrowOnError();

    LOG_INFO("Operation initialized");

    TOperationControllerInitializationResult result;
    FillInitializationResult(&result);
    return result;
}

TOperationControllerInitializationResult TOperationControllerBase::InitializeClean()
{
    LOG_INFO("Initializing operation for clean start (Title: %v)",
        Spec_->Title);

    auto initializeAction = BIND([this_ = MakeStrong(this), this] () {
        InitializeClients();
        StartTransactions();
        InitializeStructures();
        SyncPrepare();
    });

    auto initializeFuture = initializeAction
        .AsyncVia(CancelableInvoker)
        .Run()
        .WithTimeout(Config->OperationInitializationTimeout);

    WaitFor(initializeFuture)
        .ThrowOnError();

    InitUnrecognizedSpec();

    WaitFor(Host->UpdateInitializedOperationNode())
        .ThrowOnError();

    LOG_INFO("Operation initialized");

    TOperationControllerInitializationResult result;
    FillInitializationResult(&result);
    return result;
}

bool TOperationControllerBase::HasUserJobFiles() const
{
    for (const auto& userJobSpec : GetUserJobSpecs()) {
        if (!userJobSpec->FilePaths.empty() || !userJobSpec->LayerPaths.empty()) {
            return true;
        }
    }
    return false;
}

void TOperationControllerBase::InitializeStructures()
{
    InputNodeDirectory_ = New<NNodeTrackerClient::TNodeDirectory>();
    DataFlowGraph_ = New<TDataFlowGraph>(InputNodeDirectory_);
    InitializeOrchid();

    for (const auto& path : GetInputTablePaths()) {
        TInputTable table;
        table.Path = path;
        InputTables.push_back(table);
    }

    const auto& outputTablePaths = GetOutputTablePaths();
    for (int index = 0; index < outputTablePaths.size(); ++index) {
        TOutputTable table;
        table.Path = outputTablePaths[index];
        table.Options->TableIndex = index;
        auto rowCountLimit = table.Path.GetRowCountLimit();
        if (rowCountLimit) {
            if (RowCountLimitTableIndex) {
                THROW_ERROR_EXCEPTION("Only one output table with row_count_limit is supported");
            }
            RowCountLimitTableIndex = OutputTables_.size();
            RowCountLimit = rowCountLimit.Get();
        }

        Sinks_.emplace_back(std::make_unique<TSink>(this, OutputTables_.size()));
        OutputTables_.push_back(table);
    }

    if (auto stderrTablePath = GetStderrTablePath()) {
        StderrTable_.Emplace();
        StderrTable_->Path = *stderrTablePath;
        StderrTable_->OutputType = EOutputTableType::Stderr;
    }

    if (auto coreTablePath = GetCoreTablePath()) {
        CoreTable_.Emplace();
        CoreTable_->Path = *coreTablePath;
        CoreTable_->OutputType = EOutputTableType::Core;
    }

    InitUpdatingTables();

    for (const auto& userJobSpec : GetUserJobSpecs()) {
        auto& files = UserJobFiles_[userJobSpec];
        for (const auto& path : userJobSpec->FilePaths) {
            TUserFile file;
            file.Path = path;
            file.IsLayer = false;
            files.emplace_back(std::move(file));
        }

        auto layerPaths = userJobSpec->LayerPaths;
        if (Config->SystemLayerPath && !layerPaths.empty()) {
            layerPaths.emplace_back(*Config->SystemLayerPath);
        }
        for (const auto& path : layerPaths) {
            TUserFile file;
            file.Path = path;
            file.IsLayer = true;
            // This must be the top layer, so insert in the beginning.
            files.insert(files.begin(), std::move(file));
        }
    }

    auto maxInputTableCount = std::min(Config->MaxInputTableCount, Options->MaxInputTableCount);

    if (InputTables.size() > maxInputTableCount) {
        THROW_ERROR_EXCEPTION(
            "Too many input tables: maximum allowed %v, actual %v",
            Config->MaxInputTableCount,
            InputTables.size());
    }

    DoInitialize();
}

void TOperationControllerBase::InitUnrecognizedSpec()
{
    UnrecognizedSpec_ = GetTypedSpec()->GetUnrecognizedRecursively();
}

void TOperationControllerBase::FillInitializationResult(TOperationControllerInitializationResult* result)
{
    result->Attributes.Mutable = BuildYsonStringFluently<EYsonType::MapFragment>()
        .Do(BIND(&TOperationControllerBase::BuildInitializeMutableAttributes, Unretained(this)))
        .Finish();
    result->Attributes.BriefSpec = BuildYsonStringFluently<EYsonType::MapFragment>()
        .Do(BIND(&TOperationControllerBase::BuildBriefSpec, Unretained(this)))
        .Finish();
    result->Attributes.FullSpec = ConvertToYsonString(Spec_);
    result->Attributes.UnrecognizedSpec = ConvertToYsonString(UnrecognizedSpec_);
    result->Transactions = GetTransactions();
}

void TOperationControllerBase::ValidateIntermediateDataAccess(const TString& user, EPermission permission) const
{
    Host->ValidateOperationPermission(user, permission, "/intermediate_data_access");
}

void TOperationControllerBase::InitUpdatingTables()
{
    UpdatingTables.clear();

    for (auto& table : OutputTables_) {
        UpdatingTables.push_back(&table);
    }

    if (StderrTable_) {
        UpdatingTables.push_back(StderrTable_.GetPtr());
    }

    if (CoreTable_) {
        UpdatingTables.push_back(CoreTable_.GetPtr());
    }
}

void TOperationControllerBase::InitializeOrchid()
{
    auto createService = [=] (auto fluentMethod) -> IYPathServicePtr {
        return IYPathService::FromProducer(BIND([fluentMethod = std::move(fluentMethod), weakThis = MakeWeak(this)] (IYsonConsumer* consumer) {
            auto strongThis = weakThis.Lock();
            if (!strongThis) {
                THROW_ERROR_EXCEPTION(NYTree::EErrorCode::ResolveError, "Operation controller was destroyed");
            }
            BuildYsonFluently(consumer)
                .Do(fluentMethod);
        }));
    };

    // Methods like BuildProgress, BuildBriefProgress, buildJobsYson and BuildJobSplitterInfo build map fragment,
    // so we have to enclose them with a map in order to pass into createService helper.
    // TODO(max42): get rid of this when GetOperationInfo is not stopping us from changing Build* signatures any more.
    auto wrapWithMap = [=] (auto fluentMethod) {
        return [=, fluentMethod = std::move(fluentMethod)] (TFluentAny fluent) {
            fluent
                .BeginMap()
                    .Do(fluentMethod)
                .EndMap();
        };
    };

    auto createCachedMapService = [=] (auto fluentMethod) -> IYPathServicePtr {
        return createService(wrapWithMap(std::move(fluentMethod)))
            ->Via(Invoker)
            ->Cached(Config->ControllerStaticOrchidUpdatePeriod);
    };

    // NB: we may safely pass unretained this below as all the callbacks are wrapped with a createService helper
    // that takes care on checking the controller presence and properly replying in case it is already destroyed.
    auto service = New<TCompositeMapService>()
        ->AddChild("progress", createCachedMapService(BIND(&TOperationControllerBase::BuildProgress, Unretained(this))))
        ->AddChild("brief_progress", createCachedMapService(BIND(&TOperationControllerBase::BuildBriefProgress, Unretained(this))))
        ->AddChild("running_jobs", createCachedMapService(BIND(&TOperationControllerBase::BuildJobsYson, Unretained(this))))
        ->AddChild("job_splitter", createCachedMapService(BIND(&TOperationControllerBase::BuildJobSplitterInfo, Unretained(this))))
        ->AddChild("memory_usage", createService(BIND(&TOperationControllerBase::BuildMemoryUsageYson, Unretained(this))))
        ->AddChild("state", createService(BIND(&TOperationControllerBase::BuildStateYson, Unretained(this))))
        ->AddChild("data_flow_graph", DataFlowGraph_->GetService()
            ->WithPermissionValidator(BIND(&TOperationControllerBase::ValidateIntermediateDataAccess, MakeWeak(this))));
    service->SetOpaque(false);
    Orchid_ = service
        ->Via(Invoker);
}

void TOperationControllerBase::DoInitialize()
{ }

void TOperationControllerBase::SyncPrepare()
{
    PrepareInputTables();
    LockInputTables();
    LockUserFiles();
}

TOperationControllerPrepareResult TOperationControllerBase::SafePrepare()
{
    // Testing purpose code.
    if (Config->EnableControllerFailureSpecOption &&
        Spec_->TestingOperationOptions)
    {
        YCHECK(Spec_->TestingOperationOptions->ControllerFailure !=
            EControllerFailureType::AssertionFailureInPrepare);
    }

    // Process input tables.
    if (!GetInputTablePaths().empty()) {
        GetInputTablesAttributes();
    } else {
        LOG_INFO("Operation has no input tables");
    }

    PrepareInputQuery();

    // Process files.
    if (HasUserJobFiles()) {
        GetUserFilesAttributes();
    } else {
        LOG_INFO("Operation has no input files");
    }

    // Process output and stderr tables.
    if (!GetOutputTablePaths().empty()) {
        GetUserObjectBasicAttributes<TOutputTable>(
            OutputClient,
            OutputTables_,
            OutputTransaction->GetId(),
            Logger,
            EPermission::Write);
    } else {
        LOG_INFO("Operation has no output tables");
    }

    if (StderrTable_) {
        GetUserObjectBasicAttributes<TOutputTable>(
            Client,
            StderrTable_,
            DebugTransaction->GetId(),
            Logger,
            EPermission::Write);
    } else {
        LOG_INFO("Operation has no stderr table");
    }

    if (CoreTable_) {
        GetUserObjectBasicAttributes<TOutputTable>(
            Client,
            CoreTable_,
            DebugTransaction->GetId(),
            Logger,
            EPermission::Write);
    } else {
        LOG_INFO("Operation has no core table");
    }

    {
        THashSet<TObjectId> updatingTableIds;
        for (const auto* table : UpdatingTables) {
            const auto& path = table->Path.GetPath();
            if (table->Type != EObjectType::Table) {
                THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv, actual %Qlv",
                    path,
                    EObjectType::Table,
                    table->Type);
            }
            const bool insertedNew = updatingTableIds.insert(table->ObjectId).second;
            if (!insertedNew) {
                THROW_ERROR_EXCEPTION("Output table %v is specified multiple times",
                    path);
            }
        }

        GetOutputTablesSchema();
        PrepareOutputTables();

        LockOutputTablesAndGetAttributes();
    }

    InitializeStandardEdgeDescriptors();

    TOperationControllerPrepareResult result;
    FillPrepareResult(&result);
    return result;
}

void TOperationControllerBase::SafeMaterialize()
{
    try {
        FetchInputTables();
        FetchUserFiles();

        PickIntermediateDataCell();
        InitChunkListPools();

        CreateLivePreviewTables();

        CollectTotals();

        CustomPrepare();

        InitializeHistograms();

        LOG_INFO("Tasks prepared (RowBufferCapacity: %v)", RowBuffer->GetCapacity());

        if (IsCompleted()) {
            // Possible reasons:
            // - All input chunks are unavailable && Strategy == Skip
            // - Merge decided to teleport all input chunks
            // - Anything else?
            LOG_INFO("No jobs needed");
            OnOperationCompleted(false /* interrupted */);
            return;
        } else {
            YCHECK(UnavailableInputChunkCount == 0);
            for (const auto& pair : InputChunkMap) {
                const auto& chunkDescriptor = pair.second;
                if (chunkDescriptor.State == EInputChunkState::Waiting) {
                    ++UnavailableInputChunkCount;
                }
            }

            if (UnavailableInputChunkCount > 0) {
                LOG_INFO("Found unavailable input chunks during materialization (UnavailableInputChunkCount: %v)",
                    UnavailableInputChunkCount);
            }
        }

        AddAllTaskPendingHints();

        if (Config->TestingOptions->EnableSnapshotCycleAfterMaterialization) {
            TStringStream stringStream;
            SaveSnapshot(&stringStream);
            TOperationSnapshot snapshot;
            snapshot.Version = GetCurrentSnapshotVersion();
            snapshot.Blocks = {TSharedRef::FromString(stringStream.Str())};
            DoLoadSnapshot(snapshot);
        }

        // Input chunk scraper initialization should be the last step to avoid races,
        // because input chunk scraper works in control thread.
        InitInputChunkScraper();
        InitIntermediateChunkScraper();

        UpdateMinNeededJobResources();

        CheckTimeLimitExecutor->Start();
        ProgressBuildExecutor_->Start();
        ExecNodesCheckExecutor->Start();
        SuspiciousJobsYsonUpdater_->Start();
        AnalyzeOperationProgressExecutor->Start();
        MinNeededResourcesSanityCheckExecutor->Start();
        MaxAvailableExecNodeResourcesUpdateExecutor->Start();

        auto jobSplitterConfig = GetJobSplitterConfig();
        if (jobSplitterConfig) {
            JobSplitter_ = CreateJobSplitter(jobSplitterConfig, OperationId);
        }

        if (State != EControllerState::Preparing) {
            return;
        }
        State = EControllerState::Running;

        LogProgress(/* force */ true);
    } catch (const std::exception& ex) {
        auto wrappedError = TError("Materialization failed") << ex;
        LOG_ERROR(wrappedError);
        OnOperationFailed(wrappedError);
        return;
    }

    LOG_INFO("Materialization finished");
}

void TOperationControllerBase::SaveSnapshot(IOutputStream* output)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TSaveContext context;
    context.SetVersion(GetCurrentSnapshotVersion());
    context.SetOutput(output);

    Save(context, this);
}

void TOperationControllerBase::SleepInRevive()
{
    auto delay = Spec_->TestingOperationOptions->DelayInsideRevive;
    if (delay) {
        TDelayedExecutor::WaitForDuration(*delay);
    }
}

TOperationControllerReviveResult TOperationControllerBase::Revive()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    // A fast path to stop revival if fail_on_job_restart = %true and
    // this is not a vanilla operation.
    ValidateRevivalAllowed();

    if (Snapshot.Blocks.empty()) {
        LOG_INFO("Snapshot data is missing, preparing operation from scratch");
        TOperationControllerReviveResult result;
        result.RevivedFromSnapshot = false;
        static_cast<TOperationControllerPrepareResult&>(result) = Prepare();
        return result;
    }

    SleepInRevive();

    DoLoadSnapshot(Snapshot);

    // Once again check that revival is allowed (now having the loaded snapshot).
    ValidateSnapshot();

    Snapshot = TOperationSnapshot();

    TOperationControllerReviveResult result;
    result.RevivedFromSnapshot = true;
    FillPrepareResult(&result);

    InitChunkListPools();

    CreateLivePreviewTables();

    if (IsCompleted()) {
        OnOperationCompleted(/* interrupted */ false);
        return result;
    }

    AddAllTaskPendingHints();

    // Input chunk scraper initialization should be the last step to avoid races.
    InitInputChunkScraper();
    InitIntermediateChunkScraper();

    if (UnavailableIntermediateChunkCount > 0) {
        IntermediateChunkScraper->Start();
    }

    UpdateMinNeededJobResources();

    ReinstallLivePreview();

    if (!Config->EnableJobRevival) {
        AbortAllJoblets();
    }

    CheckTimeLimitExecutor->Start();
    ProgressBuildExecutor_->Start();
    ExecNodesCheckExecutor->Start();
    SuspiciousJobsYsonUpdater_->Start();
    AnalyzeOperationProgressExecutor->Start();
    MinNeededResourcesSanityCheckExecutor->Start();
    MaxAvailableExecNodeResourcesUpdateExecutor->Start();

    for (const auto& pair : JobletMap) {
        const auto& joblet = pair.second;
        result.RevivedJobs.push_back({
            joblet->JobId,
            joblet->JobType,
            joblet->StartTime,
            joblet->ResourceLimits,
            IsJobInterruptible(),
            joblet->TreeId,
            joblet->NodeDescriptor.Id,
            joblet->NodeDescriptor.Address
        });
    }

    State = EControllerState::Running;

    return result;
}

void TOperationControllerBase::AbortAllJoblets()
{
    for (const auto& pair : JobletMap) {
        auto joblet = pair.second;
        JobCounter->Aborted(1, EAbortReason::Scheduler);
        const auto& jobId = pair.first;
        auto jobSummary = TAbortedJobSummary(jobId, EAbortReason::Scheduler);
        joblet->Task->OnJobAborted(joblet, jobSummary);
        if (JobSplitter_) {
            JobSplitter_->OnJobAborted(jobSummary);
        }
    }
    JobletMap.clear();
}

bool TOperationControllerBase::IsTransactionNeeded(ETransactionType type) const
{
    switch (type) {
        case ETransactionType::Async:
            return IsIntermediateLivePreviewSupported() || IsOutputLivePreviewSupported() || GetStderrTablePath();
        case ETransactionType::Input:
            return !GetInputTablePaths().empty() || HasUserJobFiles();
        case ETransactionType::Output:
        case ETransactionType::OutputCompletion:
            return !GetOutputTablePaths().empty();
        case ETransactionType::Debug:
        case ETransactionType::DebugCompletion:
            // TODO(max42): Re-think about this transaction when YT-8270 is done.
            return true;
        default:
            Y_UNREACHABLE();
    }
}

ITransactionPtr TOperationControllerBase::AttachTransaction(const TTransactionId& transactionId, bool ping)
{
    auto connection = GetRemoteConnectionOrThrow(Host->GetClient()->GetNativeConnection(), CellTagFromId(transactionId));
    auto client = connection->CreateNativeClient(TClientOptions(NSecurityClient::SchedulerUserName));

    TTransactionAttachOptions options;
    options.Ping = ping;
    options.PingAncestors = false;
    return client->AttachTransaction(transactionId, options);
}

void TOperationControllerBase::StartTransactions()
{
    std::vector<TFuture<ITransactionPtr>> asyncResults = {
        StartTransaction(ETransactionType::Async, Client),
        StartTransaction(ETransactionType::Input, InputClient, GetInputTransactionParentId()),
        StartTransaction(ETransactionType::Output, OutputClient, GetOutputTransactionParentId()),
        // NB: we do not start Debug transaction under User transaction since we want to save debug results
        // even if user transaction is aborted.
        StartTransaction(ETransactionType::Debug, Client),
    };

    auto results = WaitFor(CombineAll(asyncResults))
        .ValueOrThrow();

    {
        AsyncTransaction = results[0].ValueOrThrow();
        InputTransaction = results[1].ValueOrThrow();
        OutputTransaction = results[2].ValueOrThrow();
        DebugTransaction = results[3].ValueOrThrow();
    }
}

TInputStreamDirectory TOperationControllerBase::GetInputStreamDirectory() const
{
    std::vector<TInputStreamDescriptor> inputStreams;
    inputStreams.reserve(InputTables.size());
    for (const auto& inputTable : InputTables) {
        inputStreams.emplace_back(inputTable.IsTeleportable, inputTable.IsPrimary(), inputTable.IsDynamic /* isVersioned */);
    }
    return TInputStreamDirectory(std::move(inputStreams));
}

IFetcherChunkScraperPtr TOperationControllerBase::CreateFetcherChunkScraper() const
{
    return Spec_->UnavailableChunkStrategy == EUnavailableChunkAction::Wait
        ? NChunkClient::CreateFetcherChunkScraper(
            Config->ChunkScraper,
            GetCancelableInvoker(),
            Host->GetChunkLocationThrottlerManager(),
            InputClient,
            InputNodeDirectory_,
            Logger)
        : nullptr;
}

TTransactionId TOperationControllerBase::GetInputTransactionParentId()
{
    return UserTransactionId;
}

TTransactionId TOperationControllerBase::GetOutputTransactionParentId()
{
    return UserTransactionId;
}

TTaskGroupPtr TOperationControllerBase::GetAutoMergeTaskGroup() const
{
    return AutoMergeTaskGroup;
}

TAutoMergeDirector* TOperationControllerBase::GetAutoMergeDirector()
{
    return AutoMergeDirector_.get();
}

TFuture<ITransactionPtr> TOperationControllerBase::StartTransaction(
    ETransactionType type,
    INativeClientPtr client,
    const TTransactionId& parentTransactionId,
    const TTransactionId& prerequisiteTransactionId)
{
    if (!IsTransactionNeeded(type)) {
        LOG_INFO("Skipping transaction as it is not needed (Type: %v)", type);
        return MakeFuture(ITransactionPtr());
    }

    LOG_INFO("Starting transaction (Type: %v, ParentId: %v, PrerequisiteTransactionId: %v)",
        type,
        parentTransactionId,
        prerequisiteTransactionId);

    TTransactionStartOptions options;
    options.AutoAbort = false;
    options.PingAncestors = false;
    auto attributes = CreateEphemeralAttributes();
    attributes->Set(
        "title",
        Format("Scheduler %Qlv transaction for operation %v",
            type,
            OperationId));
    attributes->Set("operation_id", OperationId);
    if (Spec_->Title) {
        attributes->Set("operation_title", Spec_->Title);
    }
    options.Attributes = std::move(attributes);
    options.ParentId = parentTransactionId;
    if (prerequisiteTransactionId) {
        options.PrerequisiteTransactionIds.push_back(prerequisiteTransactionId);
    }
    options.Timeout = Config->OperationTransactionTimeout;

    auto transactionFuture = client->StartTransaction(NTransactionClient::ETransactionType::Master, options);

    return transactionFuture.Apply(BIND([=] (const TErrorOr<ITransactionPtr>& transactionOrError){
        THROW_ERROR_EXCEPTION_IF_FAILED(
            transactionOrError,
            "Error starting %Qlv transaction",
            type);

        auto transaction = transactionOrError.Value();

        LOG_INFO("Transaction started (Type: %v, TransactionId: %v)",
            type,
            transaction->GetId());

        return transaction;
    }));
}

void TOperationControllerBase::PickIntermediateDataCell()
{
    auto connection = OutputClient->GetNativeConnection();
    const auto& secondaryCellTags = connection->GetSecondaryMasterCellTags();
    IntermediateOutputCellTag = secondaryCellTags.empty()
        ? connection->GetPrimaryMasterCellTag()
        : secondaryCellTags[rand() % secondaryCellTags.size()];
}

void TOperationControllerBase::InitChunkListPools()
{
    if (!GetOutputTablePaths().empty()) {
        OutputChunkListPool_ = New<TChunkListPool>(
            Config,
            OutputClient,
            CancelableInvoker,
            OperationId,
            OutputTransaction->GetId());

        CellTagToRequiredOutputChunkLists_.clear();
        for (const auto* table : UpdatingTables) {
            ++CellTagToRequiredOutputChunkLists_[table->CellTag];
        }

        ++CellTagToRequiredOutputChunkLists_[IntermediateOutputCellTag];
    }

    DebugChunkListPool_ = New<TChunkListPool>(
        Config,
        OutputClient,
        CancelableInvoker,
        OperationId,
        DebugTransaction->GetId());

    CellTagToRequiredDebugChunkLists_.clear();
    if (StderrTable_) {
        ++CellTagToRequiredDebugChunkLists_[StderrTable_->CellTag];
    }
    if (CoreTable_) {
        ++CellTagToRequiredDebugChunkLists_[CoreTable_->CellTag];
    }
}

void TOperationControllerBase::InitInputChunkScraper()
{
    THashSet<TChunkId> chunkIds;
    for (const auto& pair : InputChunkMap) {
        chunkIds.insert(pair.first);
    }

    YCHECK(!InputChunkScraper);
    InputChunkScraper = New<TChunkScraper>(
        Config->ChunkScraper,
        CancelableInvoker,
        Host->GetChunkLocationThrottlerManager(),
        InputClient,
        InputNodeDirectory_,
        std::move(chunkIds),
        BIND(&TThis::OnInputChunkLocated, MakeWeak(this)),
        Logger);

    if (UnavailableInputChunkCount > 0) {
        LOG_INFO("Waiting for %v unavailable input chunks", UnavailableInputChunkCount);
        InputChunkScraper->Start();
    }
}

void TOperationControllerBase::InitIntermediateChunkScraper()
{
    IntermediateChunkScraper = New<TIntermediateChunkScraper>(
        Config->ChunkScraper,
        CancelableInvoker,
        Host->GetChunkLocationThrottlerManager(),
        InputClient,
        InputNodeDirectory_,
        [weakThis = MakeWeak(this)] () {
            if (auto this_ = weakThis.Lock()) {
                return this_->GetAliveIntermediateChunks();
            } else {
                return THashSet<TChunkId>();
            }
        },
        BIND(&TThis::OnIntermediateChunkLocated, MakeWeak(this)),
        Logger);
}

bool TOperationControllerBase::TryInitAutoMerge(int outputChunkCountEstimate, double dataWeightRatio)
{
    InitAutoMergeJobSpecTemplates();

    AutoMergeTaskGroup = New<TTaskGroup>();
    AutoMergeTaskGroup->MinNeededResources.SetCpu(1);

    RegisterTaskGroup(AutoMergeTaskGroup);

    const auto& autoMergeSpec = Spec_->AutoMerge;
    auto mode = autoMergeSpec->Mode;
    if (outputChunkCountEstimate <= 1) {
        LOG_INFO("Output chunk count estimate does not exceed 1, force disabling auto merge "
            "(OutputChunkCountEstimate: %v)",
            outputChunkCountEstimate);
        return false;
    }

    if (mode == EAutoMergeMode::Disabled) {
        return false;
    }

    AutoMergeTasks.reserve(OutputTables_.size());
    i64 maxIntermediateChunkCount;
    i64 chunkCountPerMergeJob;
    switch (mode) {
        case EAutoMergeMode::Relaxed:
            maxIntermediateChunkCount = std::numeric_limits<int>::max();
            chunkCountPerMergeJob = 500;
            break;
        case EAutoMergeMode::Economy:
            maxIntermediateChunkCount = std::max(500, static_cast<int>(2.5 * sqrt(outputChunkCountEstimate)));
            chunkCountPerMergeJob = maxIntermediateChunkCount / 10;
            break;
        case EAutoMergeMode::Manual:
            maxIntermediateChunkCount = *autoMergeSpec->MaxIntermediateChunkCount;
            chunkCountPerMergeJob = *autoMergeSpec->ChunkCountPerMergeJob;
            break;
        default:
            Y_UNREACHABLE();
    }
    i64 desiredChunkSize = autoMergeSpec->JobIO->TableWriter->DesiredChunkSize;
    i64 desiredChunkDataWeight = desiredChunkSize / dataWeightRatio;
    i64 dataWeightPerJob = std::min(1_GB, desiredChunkDataWeight);

    // NB: if row count limit is set on any output table, we do not
    // enable auto merge as it prematurely stops the operation
    // because wrong statistics are currently used when checking row count.
    for (int index = 0; index < OutputTables_.size(); ++index) {
        if (OutputTables_[index].Path.GetRowCountLimit()) {
            LOG_INFO("Output table has row count limit, force disabling auto merge (TableIndex: %v)", index);
            return false;
        }
    }

    LOG_INFO("Auto merge parameters calculated ("
        "Mode: %v, OutputChunkCountEstimate: %v, MaxIntermediateChunkCount: %v, ChunkCountPerMergeJob: %v,"
        "ChunkSizeThreshold: %v, DesiredChunkSize: %v, DesiredChunkDataWeight: %v, IntermediateChunkUnstageMode: %v)",
        mode,
        outputChunkCountEstimate,
        maxIntermediateChunkCount,
        chunkCountPerMergeJob,
        autoMergeSpec->ChunkSizeThreshold,
        desiredChunkSize,
        desiredChunkDataWeight,
        GetIntermediateChunkUnstageMode());

    AutoMergeDirector_ = std::make_unique<TAutoMergeDirector>(
        maxIntermediateChunkCount,
        chunkCountPerMergeJob,
        OperationId);

    bool autoMergeEnabled = false;

    auto standardEdgeDescriptors = GetStandardEdgeDescriptors();
    for (int index = 0; index < OutputTables_.size(); ++index) {
        const auto& outputTable = OutputTables_[index];
        if (outputTable.Path.GetAutoMerge() &&
            !outputTable.TableUploadOptions.TableSchema.IsSorted())
        {
            auto edgeDescriptor = standardEdgeDescriptors[index];
            // Auto-merge jobs produce single output, so we override the table
            // index in writer options with 0.
            edgeDescriptor.TableWriterOptions = CloneYsonSerializable(edgeDescriptor.TableWriterOptions);
            edgeDescriptor.TableWriterOptions->TableIndex = 0;
            auto task = New<TAutoMergeTask>(
                this /* taskHost */,
                index,
                chunkCountPerMergeJob,
                autoMergeSpec->ChunkSizeThreshold,
                dataWeightPerJob,
                Spec_->MaxDataWeightPerJob,
                edgeDescriptor);
            RegisterTask(task);
            AutoMergeTasks.emplace_back(std::move(task));
            autoMergeEnabled = true;
        } else {
            AutoMergeTasks.emplace_back(nullptr);
        }
    }

    return autoMergeEnabled;
}

std::vector<TEdgeDescriptor> TOperationControllerBase::GetAutoMergeEdgeDescriptors()
{
    auto edgeDescriptors = GetStandardEdgeDescriptors();
    YCHECK(GetAutoMergeDirector());
    YCHECK(AutoMergeTasks.size() == edgeDescriptors.size());
    for (int index = 0; index < edgeDescriptors.size(); ++index) {
        if (AutoMergeTasks[index]) {
            edgeDescriptors[index].DestinationPool = AutoMergeTasks[index]->GetChunkPoolInput();
            edgeDescriptors[index].ChunkMapping = AutoMergeTasks[index]->GetChunkMapping();
            edgeDescriptors[index].ImmediatelyUnstageChunkLists = true;
            edgeDescriptors[index].RequiresRecoveryInfo = true;
            edgeDescriptors[index].IsFinalOutput = false;
        }
    }
    return edgeDescriptors;
}

THashSet<TChunkId> TOperationControllerBase::GetAliveIntermediateChunks() const
{
    THashSet<TChunkId> intermediateChunks;

    for (const auto& pair : ChunkOriginMap) {
        if (!pair.second->Suspended || !pair.second->Restartable) {
            intermediateChunks.insert(pair.first);
        }
    }

    return intermediateChunks;
}

void TOperationControllerBase::ReinstallLivePreview()
{
    if (IsOutputLivePreviewSupported()) {
        for (const auto& table : OutputTables_) {
            std::vector<TChunkTreeId> childIds;
            childIds.reserve(table.OutputChunkTreeIds.size());
            for (const auto& pair : table.OutputChunkTreeIds) {
                childIds.push_back(pair.second);
            }
            Host->AttachChunkTreesToLivePreview(
                AsyncTransaction->GetId(),
                table.LivePreviewTableId,
                childIds);
        }
    }

    if (IsIntermediateLivePreviewSupported()) {
        std::vector<TChunkTreeId> childIds;
        childIds.reserve(ChunkOriginMap.size());
        for (const auto& pair : ChunkOriginMap) {
            if (!pair.second->Suspended) {
                childIds.push_back(pair.first);
            }
        }
        Host->AttachChunkTreesToLivePreview(
            AsyncTransaction->GetId(),
            IntermediateTable.LivePreviewTableId,
            childIds);
    }
}

void TOperationControllerBase::DoLoadSnapshot(const TOperationSnapshot& snapshot)
{
    LOG_INFO("Started loading snapshot (Size: %v, BlockCount: %v, Version: %v)",
        GetByteSize(snapshot.Blocks),
        snapshot.Blocks.size(),
        snapshot.Version);

    TChunkedInputStream input(snapshot.Blocks);

    TLoadContext context;
    context.SetInput(&input);
    context.SetRowBuffer(RowBuffer);
    context.SetVersion(snapshot.Version);

    NPhoenix::TSerializer::InplaceLoad(context, this);

    LOG_INFO("Finished loading snapshot");
}

void TOperationControllerBase::StartOutputCompletionTransaction()
{
    if (!OutputTransaction) {
        return;
    }

    OutputCompletionTransaction = WaitFor(StartTransaction(
        ETransactionType::OutputCompletion,
        OutputClient,
        OutputTransaction->GetId(),
        Host->GetIncarnationId()))
        .ValueOrThrow();

    // Set transaction id to Cypress.
    {
        const auto& client = Host->GetClient();
        auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        auto path = GetNewOperationPath(OperationId) + "/@output_completion_transaction_id";
        auto req = TYPathProxy::Set(path);
        req->set_value(ConvertToYsonString(OutputCompletionTransaction->GetId()).GetData());
        WaitFor(proxy.Execute(req))
            .ThrowOnError();
    }
}

void TOperationControllerBase::CommitOutputCompletionTransaction()
{
    // Set committed flag.
    {
        const auto& client = Host->GetClient();
        auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        auto path = GetNewOperationPath(OperationId) + "/@committed";
        auto req = TYPathProxy::Set(path);
        SetTransactionId(req, OutputCompletionTransaction ? OutputCompletionTransaction->GetId() : NullTransactionId);
        req->set_value(ConvertToYsonString(true).GetData());
        WaitFor(proxy.Execute(req))
            .ThrowOnError();
    }

    if (OutputCompletionTransaction) {
        WaitFor(OutputCompletionTransaction->Commit())
            .ThrowOnError();
        OutputCompletionTransaction.Reset();
    }

    CommitFinished = true;
}

void TOperationControllerBase::StartDebugCompletionTransaction()
{
    if (!DebugTransaction) {
        return;
    }

    DebugCompletionTransaction = WaitFor(StartTransaction(
        ETransactionType::DebugCompletion,
        OutputClient,
        DebugTransaction->GetId(),
        Host->GetIncarnationId()))
        .ValueOrThrow();

    // Set transaction id to Cypress.
    {
        const auto& client = Host->GetClient();
        auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        auto path = GetNewOperationPath(OperationId) + "/@debug_completion_transaction_id";
        auto req = TYPathProxy::Set(path);
        req->set_value(ConvertToYsonString(DebugCompletionTransaction->GetId()).GetData());
        WaitFor(proxy.Execute(req))
            .ThrowOnError();
    }
}

void TOperationControllerBase::CommitDebugCompletionTransaction()
{
    if (!DebugTransaction) {
        return;
    }

    WaitFor(DebugCompletionTransaction->Commit())
        .ThrowOnError();
    DebugCompletionTransaction.Reset();
}

void TOperationControllerBase::SleepInCommitStage(EDelayInsideOperationCommitStage desiredStage)
{
    auto delay = Spec_->TestingOperationOptions->DelayInsideOperationCommit;
    auto stage = Spec_->TestingOperationOptions->DelayInsideOperationCommitStage;

    if (delay && stage && *stage == desiredStage) {
        TDelayedExecutor::WaitForDuration(*delay);
    }
}

i64 TOperationControllerBase::GetPartSize(EOutputTableType tableType)
{
    if (tableType == EOutputTableType::Stderr) {
        return GetStderrTableWriterConfig()->MaxPartSize;
    }
    if (tableType == EOutputTableType::Core) {
        return GetCoreTableWriterConfig()->MaxPartSize;
    }

    Y_UNREACHABLE();
}

void TOperationControllerBase::SafeCommit()
{
    StartOutputCompletionTransaction();
    StartDebugCompletionTransaction();

    SleepInCommitStage(EDelayInsideOperationCommitStage::Stage1);
    BeginUploadOutputTables(UpdatingTables);
    SleepInCommitStage(EDelayInsideOperationCommitStage::Stage2);
    TeleportOutputChunks();
    SleepInCommitStage(EDelayInsideOperationCommitStage::Stage3);
    AttachOutputChunks(UpdatingTables);
    SleepInCommitStage(EDelayInsideOperationCommitStage::Stage4);
    EndUploadOutputTables(UpdatingTables);
    SleepInCommitStage(EDelayInsideOperationCommitStage::Stage5);

    CustomCommit();

    CommitOutputCompletionTransaction();
    CommitDebugCompletionTransaction();
    SleepInCommitStage(EDelayInsideOperationCommitStage::Stage6);
    CommitTransactions();

    CancelableContext->Cancel();

    LOG_INFO("Results committed");
}

void TOperationControllerBase::CommitTransactions()
{
    LOG_INFO("Committing scheduler transactions");

    std::vector<TFuture<TTransactionCommitResult>> commitFutures;

    if (OutputTransaction) {
        commitFutures.push_back(OutputTransaction->Commit());
    }

    SleepInCommitStage(EDelayInsideOperationCommitStage::Stage7);

    if (DebugTransaction) {
        commitFutures.push_back(DebugTransaction->Commit());
    }

    WaitFor(Combine(commitFutures))
        .ThrowOnError();

    LOG_INFO("Scheduler transactions committed");

    // Fire-and-forget.
    if (InputTransaction) {
        InputTransaction->Abort();
    }
    if (AsyncTransaction) {
        AsyncTransaction->Abort();
    }
}

void TOperationControllerBase::TeleportOutputChunks()
{
    if (GetOutputTablePaths().empty()) {
        return;
    }

    auto teleporter = New<TChunkTeleporter>(
        Config,
        OutputClient,
        CancelableInvoker,
        OutputCompletionTransaction->GetId(),
        Logger);

    for (auto& table : OutputTables_) {
        for (const auto& pair : table.OutputChunkTreeIds) {
            const auto& id = pair.second;
            if (TypeFromId(id) == EObjectType::ChunkList)
                continue;
            teleporter->RegisterChunk(id, table.CellTag);
        }
    }

    WaitFor(teleporter->Run())
        .ThrowOnError();
}

void TOperationControllerBase::AttachOutputChunks(const std::vector<TOutputTable*>& tableList)
{
    for (auto* table : tableList) {
        auto objectIdPath = FromObjectId(table->ObjectId);
        const auto& path = table->Path.GetPath();

        LOG_INFO("Attaching output chunks (Path: %v)",
            path);

        auto channel = OutputClient->GetMasterChannelOrThrow(
            EMasterChannelKind::Leader,
            table->CellTag);
        TChunkServiceProxy proxy(channel);

        // Split large outputs into separate requests.
        TChunkServiceProxy::TReqExecuteBatch::TAttachChunkTreesSubrequest* req = nullptr;
        TChunkServiceProxy::TReqExecuteBatchPtr batchReq;

        auto flushCurrentReq = [&] (bool requestStatistics) {
            if (req) {
                req->set_request_statistics(requestStatistics);

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error attaching chunks to output table %v",
                    path);

                const auto& batchRsp = batchRspOrError.Value();
                const auto& rsp = batchRsp->attach_chunk_trees_subresponses(0);
                if (requestStatistics) {
                    table->DataStatistics = rsp.statistics();
                }
            }

            req = nullptr;
            batchReq.Reset();
        };

        auto addChunkTree = [&] (const TChunkTreeId& chunkTreeId) {
            if (req && req->child_ids_size() >= Config->MaxChildrenPerAttachRequest) {
                // NB: No need for a statistics for an intermediate request.
                flushCurrentReq(false);
            }

            if (!req) {
                batchReq = proxy.ExecuteBatch();
                GenerateMutationId(batchReq);
                batchReq->set_suppress_upstream_sync(true);
                req = batchReq->add_attach_chunk_trees_subrequests();
                ToProto(req->mutable_parent_id(), table->OutputChunkListId);
            }

            ToProto(req->add_child_ids(), chunkTreeId);
        };

        if (table->TableUploadOptions.TableSchema.IsSorted() && ShouldVerifySortedOutput()) {
            // Sorted output generated by user operation requires rearranging.
            LOG_DEBUG("Sorting output chunk tree ids by boundary keys (ChunkTreeCount: %v, Table: %v)",
                table->OutputChunkTreeIds.size(),
                path);
            std::stable_sort(
                table->OutputChunkTreeIds.begin(),
                table->OutputChunkTreeIds.end(),
                [&] (const auto& lhs, const auto& rhs) -> bool {
                    auto lhsBoundaryKeys = lhs.first.AsBoundaryKeys();
                    auto rhsBoundaryKeys = rhs.first.AsBoundaryKeys();
                    auto minKeyResult = CompareRows(lhsBoundaryKeys.MinKey, rhsBoundaryKeys.MinKey);
                    if (minKeyResult != 0) {
                        return minKeyResult < 0;
                    }
                    return lhsBoundaryKeys.MaxKey < rhsBoundaryKeys.MaxKey;
                });

            for (auto current = table->OutputChunkTreeIds.begin(); current != table->OutputChunkTreeIds.end(); ++current) {
                auto next = current + 1;
                if (next != table->OutputChunkTreeIds.end()) {
                    int cmp = CompareRows(next->first.AsBoundaryKeys().MinKey, current->first.AsBoundaryKeys().MaxKey);

                    if (cmp < 0) {
                        THROW_ERROR_EXCEPTION("Output table %v is not sorted: job outputs have overlapping key ranges",
                            table->Path.GetPath())
                            << TErrorAttribute("current_range_max_key", current->first.AsBoundaryKeys().MaxKey)
                            << TErrorAttribute("next_range_min_key", next->first.AsBoundaryKeys().MinKey);
                    }

                    if (cmp == 0 && table->Options->ValidateUniqueKeys) {
                        THROW_ERROR_EXCEPTION("Output table %v contains duplicate keys: job outputs have overlapping key ranges",
                            table->Path.GetPath())
                            << TErrorAttribute("current_range_max_key", current->first.AsBoundaryKeys().MaxKey)
                            << TErrorAttribute("next_range_min_key", next->first.AsBoundaryKeys().MinKey);
                    }
                }

                addChunkTree(current->second);
            }
        } else if (auto outputOrder = GetOutputOrder()) {
            LOG_DEBUG("Sorting output chunk tree ids according to a given output order (ChunkTreeCount: %v, Table: %v)",
                table->OutputChunkTreeIds.size(),
                path);
            std::vector<std::pair<TOutputOrder::TEntry, TChunkTreeId>> chunkTreeIds;
            for (auto& pair : table->OutputChunkTreeIds) {
                chunkTreeIds.emplace_back(std::move(pair.first.AsOutputOrderEntry()), pair.second);
            }

            auto outputChunkTreeIds = outputOrder->ArrangeOutputChunkTrees(std::move(chunkTreeIds));
            for (const auto& chunkTreeId : outputChunkTreeIds) {
                addChunkTree(chunkTreeId);
            }
        } else {
            LOG_DEBUG("Sorting output chunk tree ids by integer keys (ChunkTreeCount: %v, Table: %v)",
                table->OutputChunkTreeIds.size(), path);
            std::stable_sort(
                table->OutputChunkTreeIds.begin(),
                table->OutputChunkTreeIds.end(),
                [&] (const auto& lhs, const auto& rhs) -> bool {
                    auto lhsKey = lhs.first.AsIndex();
                    auto rhsKey = rhs.first.AsIndex();
                    return lhsKey < rhsKey;
                }
            );
            for (const auto& pair : table->OutputChunkTreeIds) {
                addChunkTree(pair.second);
            }
        }

        // NB: Don't forget to ask for the statistics in the last request.
        flushCurrentReq(true);

        LOG_INFO("Output chunks attached (Path: %v, Statistics: %v)",
            path,
            table->DataStatistics);
    }
}

void TOperationControllerBase::CustomCommit()
{ }

void TOperationControllerBase::EndUploadOutputTables(const std::vector<TOutputTable*>& tableList)
{
    auto channel = OutputClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto* table : tableList) {
        auto objectIdPath = FromObjectId(table->ObjectId);
        const auto& path = table->Path.GetPath();

        LOG_INFO("Finishing upload to output table (Path: %v, Schema: %v)",
            path,
            table->TableUploadOptions.TableSchema);

        {
            auto req = TTableYPathProxy::EndUpload(objectIdPath);
            *req->mutable_statistics() = table->DataStatistics;
            ToProto(req->mutable_table_schema(), table->TableUploadOptions.TableSchema);
            req->set_schema_mode(static_cast<int>(table->TableUploadOptions.SchemaMode));
            req->set_optimize_for(static_cast<int>(table->TableUploadOptions.OptimizeFor));
            req->set_compression_codec(static_cast<int>(table->TableUploadOptions.CompressionCodec));
            req->set_erasure_codec(static_cast<int>(table->TableUploadOptions.ErasureCodec));

            SetTransactionId(req, table->UploadTransactionId);
            GenerateMutationId(req);
            batchReq->AddRequest(req, "end_upload");
        }

        if (table->OutputType == EOutputTableType::Stderr || table->OutputType == EOutputTableType::Core) {
            auto attributesPath = path + "/@part_size";
            auto req = TYPathProxy::Set(attributesPath);
            SetTransactionId(req, GetTransactionForOutputTable(*table)->GetId());
            req->set_value(ConvertToYsonString(GetPartSize(table->OutputType)).GetData());
            batchReq->AddRequest(req, "set_part_size");
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error finishing upload to output tables");
}

void TOperationControllerBase::SafeOnJobStarted(std::unique_ptr<TStartedJobSummary> jobSummary)
{
    auto jobId = jobSummary->Id;

    if (State != EControllerState::Running) {
        LOG_DEBUG("Stale job started, ignored (JobId: %v)", jobId);
        return;
    }

    LOG_DEBUG("Job started (JobId: %v)", jobId);

    auto joblet = GetJoblet(jobId);
    joblet->LastActivityTime = jobSummary->StartTime;

    LogEventFluently(ELogEventType::JobStarted)
        .Item("job_id").Value(jobId)
        .Item("operation_id").Value(OperationId)
        .Item("resource_limits").Value(joblet->ResourceLimits)
        .Item("node_address").Value(joblet->NodeDescriptor.Address)
        .Item("job_type").Value(joblet->JobType);

    LogProgress();
}

void TOperationControllerBase::UpdateMemoryDigests(TJobletPtr joblet, const TStatistics& statistics, bool resourceOverdraft)
{
    bool taskUpdateNeeded = false;

    auto userJobMaxMemoryUsage = FindNumericValue(statistics, "/user_job/max_memory");
    if (userJobMaxMemoryUsage) {
        auto* digest = joblet->Task->GetUserJobMemoryDigest();
        YCHECK(digest);
        double actualFactor = static_cast<double>(*userJobMaxMemoryUsage) / joblet->EstimatedResourceUsage.GetUserJobMemory();
        if (resourceOverdraft) {
            // During resource overdraft actual max memory values may be outdated,
            // since statistics are updated periodically. To ensure that digest converge to large enough
            // values we introduce additional factor.
            actualFactor = std::max(actualFactor, *joblet->UserJobMemoryReserveFactor * Config->ResourceOverdraftFactor);
        }
        LOG_TRACE("Adding sample to the job proxy memory digest (JobType: %v, Sample: %v, JobId: %v)",
            joblet->JobType,
            actualFactor,
            joblet->JobId);
        digest->AddSample(actualFactor);
        taskUpdateNeeded = true;
    }

    auto jobProxyMaxMemoryUsage = FindNumericValue(statistics, "/job_proxy/max_memory");
    if (jobProxyMaxMemoryUsage) {
        auto* digest = joblet->Task->GetJobProxyMemoryDigest();
        YCHECK(digest);
        double actualFactor = static_cast<double>(*jobProxyMaxMemoryUsage) /
            (joblet->EstimatedResourceUsage.GetJobProxyMemory() + joblet->EstimatedResourceUsage.GetFootprintMemory());
        if (resourceOverdraft) {
            actualFactor = std::max(actualFactor, *joblet->JobProxyMemoryReserveFactor * Config->ResourceOverdraftFactor);
        }
        LOG_TRACE("Adding sample to the user job memory digest (JobType: %v, Sample: %v, JobId: %v)",
            joblet->JobType,
            actualFactor,
            joblet->JobId);
        digest->AddSample(actualFactor);
        taskUpdateNeeded = true;
    }

    if (taskUpdateNeeded) {
        UpdateAllTasksIfNeeded();
    }
}

void TOperationControllerBase::InitializeHistograms()
{
    if (IsInputDataSizeHistogramSupported()) {
        EstimatedInputDataSizeHistogram_ = CreateHistogram();
        InputDataSizeHistogram_ = CreateHistogram();
    }
}

void TOperationControllerBase::AddValueToEstimatedHistogram(const TJobletPtr& joblet)
{
    if (EstimatedInputDataSizeHistogram_) {
        EstimatedInputDataSizeHistogram_->AddValue(joblet->InputStripeList->TotalDataWeight);
    }
}

void TOperationControllerBase::RemoveValueFromEstimatedHistogram(const TJobletPtr& joblet)
{
    if (EstimatedInputDataSizeHistogram_) {
        EstimatedInputDataSizeHistogram_->RemoveValue(joblet->InputStripeList->TotalDataWeight);
    }
}

void TOperationControllerBase::UpdateActualHistogram(const TStatistics& statistics)
{
    if (InputDataSizeHistogram_) {
        auto dataWeight = FindNumericValue(statistics, "/data/input/data_weight");
        if (dataWeight && *dataWeight > 0) {
            InputDataSizeHistogram_->AddValue(*dataWeight);
        }
    }
}

void TOperationControllerBase::SafeOnJobCompleted(std::unique_ptr<TCompletedJobSummary> jobSummary)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto jobId = jobSummary->Id;
    auto abandoned = jobSummary->Abandoned;

    // NB: We should not explicitly tell node to remove abandoned job because it may be still
    // running at the node.
    if (!abandoned) {
        CompletedJobIdsReleaseQueue_.Push(jobId);
    }

    // Testing purpose code.
    if (Config->EnableControllerFailureSpecOption && Spec_->TestingOperationOptions &&
        Spec_->TestingOperationOptions->ControllerFailure == EControllerFailureType::ExceptionThrownInOnJobCompleted)
    {
        THROW_ERROR_EXCEPTION("Testing exception");
    }

    if (State != EControllerState::Running) {
        LOG_DEBUG("Stale job completed, ignored (JobId: %v)", jobId);
        return;
    }

    LOG_DEBUG("Job completed (JobId: %v)", jobId);

    const auto& result = jobSummary->Result;

    const auto& schedulerResultExt = result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    // Validate all node ids of the output chunks and populate the local node directory.
    // In case any id is not known, abort the job.
    const auto& globalNodeDirectory = Host->GetNodeDirectory();
    for (const auto& chunkSpec : schedulerResultExt.output_chunk_specs()) {
        auto replicas = FromProto<TChunkReplicaList>(chunkSpec.replicas());
        for (auto replica : replicas) {
            auto nodeId = replica.GetNodeId();
            if (InputNodeDirectory_->FindDescriptor(nodeId)) {
                continue;
            }

            const auto* descriptor = globalNodeDirectory->FindDescriptor(nodeId);
            if (!descriptor) {
                LOG_DEBUG("Job is considered aborted since its output contains unresolved node id "
                    "(JobId: %v, NodeId: %v)",
                    jobId,
                    nodeId);
                auto abortedJobSummary = std::make_unique<TAbortedJobSummary>(*jobSummary, EAbortReason::Other);
                OnJobAborted(std::move(abortedJobSummary), false /* byScheduler */);
                return;
            }

            InputNodeDirectory_->AddDescriptor(nodeId, *descriptor);
        }
    }

    if (jobSummary->InterruptReason != EInterruptReason::None) {
        ExtractInterruptDescriptor(*jobSummary);
    }

    JobCounter->Completed(1, jobSummary->InterruptReason);

    auto joblet = GetJoblet(jobId);

    ParseStatistics(jobSummary.get(), joblet->StatisticsYson);

    const auto& statistics = *jobSummary->Statistics;

    UpdateMemoryDigests(joblet, statistics);
    UpdateActualHistogram(statistics);

    FinalizeJoblet(joblet, jobSummary.get());
    LogFinishedJobFluently(ELogEventType::JobCompleted, joblet, *jobSummary);

    UpdateJobStatistics(joblet, *jobSummary);
    UpdateJobMetrics(joblet, *jobSummary);

    if (jobSummary->InterruptReason != EInterruptReason::None) {
        jobSummary->SplitJobCount = EstimateSplitJobCount(*jobSummary, joblet);
        LOG_DEBUG("Job interrupted (JobId: %v, InterruptReason: %v, UnreadDataSliceCount: %v, SplitJobCount: %v)",
            jobSummary->Id,
            jobSummary->InterruptReason,
            jobSummary->UnreadInputDataSlices.size(),
            jobSummary->SplitJobCount);
    }
    auto taskResult = joblet->Task->OnJobCompleted(joblet, *jobSummary);
    if (taskResult.BanTree) {
        Host->OnOperationBannedInTentativeTree(joblet->TreeId);
    }

    if (JobSplitter_) {
        JobSplitter_->OnJobCompleted(*jobSummary);
    }

    if (!abandoned && JobSpecCompletedArchiveCount_ < Config->MaxArchivedJobSpecCountPerOperation) {
        ++JobSpecCompletedArchiveCount_;
        jobSummary->ArchiveJobSpec = true;
    }

    // Statistics job state saved from jobSummary before moving jobSummary to ProcessFinishedJobResult.
    auto statisticsState = GetStatisticsJobState(joblet, jobSummary->State);

    ProcessFinishedJobResult(std::move(jobSummary), /* requestJobNodeCreation */ false);

    UnregisterJoblet(joblet);

    UpdateTask(joblet->Task);

    LogProgress();

    if (IsCompleted()) {
        OnOperationCompleted(/* interrupted */ false);
        return;
    }

    auto statisticsSuffix = JobHelper.GetStatisticsSuffix(statisticsState, joblet->JobType);

    if (RowCountLimitTableIndex) {
        switch (joblet->JobType) {
            case EJobType::Map:
            case EJobType::OrderedMap:
            case EJobType::SortedReduce:
            case EJobType::JoinReduce:
            case EJobType::PartitionReduce: {
                auto path = Format("/data/output/%v/row_count%v", *RowCountLimitTableIndex, statisticsSuffix);
                i64 count = GetNumericValue(JobStatistics, path);
                if (count >= RowCountLimit) {
                    OnOperationCompleted(true /* interrupted */);
                }
                break;
            }
            default:
                break;
        }
    }

    CheckFailedJobsStatusReceived();
}

void TOperationControllerBase::SafeOnJobFailed(std::unique_ptr<TFailedJobSummary> jobSummary)
{
    auto jobId = jobSummary->Id;
    const auto& result = jobSummary->Result;

    auto error = FromProto<TError>(result.error());

    JobCounter->Failed(1);

    auto joblet = GetJoblet(jobId);

    ParseStatistics(jobSummary.get(), joblet->StatisticsYson);

    FinalizeJoblet(joblet, jobSummary.get());
    LogFinishedJobFluently(ELogEventType::JobFailed, joblet, *jobSummary)
        .Item("error").Value(error);

    UpdateJobMetrics(joblet, *jobSummary);
    UpdateJobStatistics(joblet, *jobSummary);

    joblet->Task->OnJobFailed(joblet, *jobSummary);
    if (JobSplitter_) {
        JobSplitter_->OnJobFailed(*jobSummary);
    }

    jobSummary->ArchiveJobSpec = true;

    ProcessFinishedJobResult(std::move(jobSummary), /* requestJobNodeCreation */ true);

    UnregisterJoblet(joblet);

    LogProgress();

    if (error.Attributes().Get<bool>("fatal", false)) {
        auto wrappedError = TError("Job failed with fatal error") << error;
        OnOperationFailed(wrappedError);
        return;
    }

    int failedJobCount = JobCounter->GetFailed();
    int maxFailedJobCount = Spec_->MaxFailedJobCount;
    if (failedJobCount >= maxFailedJobCount) {
        OnOperationFailed(TError("Failed jobs limit exceeded")
            << TErrorAttribute("max_failed_job_count", maxFailedJobCount));
    }

    CheckFailedJobsStatusReceived();

    if (Spec_->FailOnJobRestart) {
        OnOperationFailed(TError(NScheduler::EErrorCode::OperationFailedOnJobRestart,
            "Job failed; operation failed because spec option fail_on_job_restart is set")
            << TErrorAttribute("job_id", joblet->JobId)
            << error);
    }

    ReleaseJobs({jobId});
}

void TOperationControllerBase::SafeOnJobAborted(std::unique_ptr<TAbortedJobSummary> jobSummary, bool byScheduler)
{
    auto jobId = jobSummary->Id;
    auto abortReason = jobSummary->AbortReason;

    if (State != EControllerState::Running) {
        LOG_DEBUG("Stale job aborted, ignored (JobId: %v)", jobId);
        return;
    }

    LOG_DEBUG("Job aborted (JobId: %v)", jobId);

    JobCounter->Aborted(1, abortReason);

    auto joblet = GetJoblet(jobId);

    ParseStatistics(jobSummary.get(), joblet->StatisticsYson);
    const auto& statistics = *jobSummary->Statistics;

    if (abortReason == EAbortReason::ResourceOverdraft) {
        UpdateMemoryDigests(joblet, statistics, true /* resourceOverdraft */);
    }

    if (jobSummary->LogAndProfile) {
        FinalizeJoblet(joblet, jobSummary.get());
        LogFinishedJobFluently(ELogEventType::JobAborted, joblet, *jobSummary)
            .Item("reason").Value(abortReason);
        UpdateJobStatistics(joblet, *jobSummary);
    }

    UpdateJobMetrics(joblet, *jobSummary);

    if (abortReason == EAbortReason::FailedChunks) {
        const auto& result = jobSummary->Result;
        const auto& schedulerResultExt = result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
        for (const auto& chunkId : schedulerResultExt.failed_chunk_ids()) {
            OnChunkFailed(FromProto<TChunkId>(chunkId));
        }
    }

    joblet->Task->OnJobAborted(joblet, *jobSummary);

    if (JobSplitter_) {
        JobSplitter_->OnJobAborted(*jobSummary);
    }

    bool requestJobNodeCreation = (abortReason == EAbortReason::UserRequest);
    ProcessFinishedJobResult(std::move(jobSummary), requestJobNodeCreation);

    UnregisterJoblet(joblet);

    if (abortReason == EAbortReason::AccountLimitExceeded) {
        Host->OnOperationSuspended(TError("Account limit exceeded"));
    }

    CheckFailedJobsStatusReceived();
    LogProgress();

    if (Spec_->FailOnJobRestart &&
        !(abortReason > EAbortReason::SchedulingFirst && abortReason < EAbortReason::SchedulingLast))
    {
        OnOperationFailed(TError(
            NScheduler::EErrorCode::OperationFailedOnJobRestart,
            "Job aborted; operation failed because spec option fail_on_job_restart is set")
            << TErrorAttribute("job_id", joblet->JobId)
            << TErrorAttribute("abort_reason", abortReason));
    }

    if (!byScheduler) {
        ReleaseJobs({jobId});
    }
}

void TOperationControllerBase::SafeOnJobRunning(std::unique_ptr<TRunningJobSummary> jobSummary)
{
    const auto& jobId = jobSummary->Id;

    if (State != EControllerState::Running) {
        LOG_DEBUG("Stale job running, ignored (JobId: %v)", jobId);
        return;
    }

    auto joblet = GetJoblet(jobSummary->Id);

    joblet->Progress = jobSummary->Progress;
    joblet->StderrSize = jobSummary->StderrSize;

    if (jobSummary->StatisticsYson) {
        joblet->StatisticsYson = jobSummary->StatisticsYson;
        ParseStatistics(jobSummary.get());

        UpdateJobMetrics(joblet, *jobSummary);

        if (JobSplitter_) {
            JobSplitter_->OnJobRunning(*jobSummary);
            if (GetPendingJobCount() == 0 && JobSplitter_->IsJobSplittable(jobId)) {
                LOG_DEBUG("Job is ready to be split (JobId: %v)", jobId);
                Host->InterruptJob(jobId, EInterruptReason::JobSplit);
            }
        }

        auto asyncResult = BIND(&BuildBriefStatistics, Passed(std::move(jobSummary)))
            .AsyncVia(Host->GetControllerThreadPoolInvoker())
            .Run();

        asyncResult.Subscribe(BIND(
            &TOperationControllerBase::AnalyzeBriefStatistics,
            MakeStrong(this),
            joblet,
            Config->SuspiciousJobs)
            .Via(GetInvoker()));
    }
}

void TOperationControllerBase::FinalizeJoblet(
    const TJobletPtr& joblet,
    TJobSummary* jobSummary)
{
    YCHECK(jobSummary->Statistics);
    YCHECK(jobSummary->FinishTime);

    auto& statistics = *jobSummary->Statistics;
    joblet->FinishTime = *jobSummary->FinishTime;

    {
        auto duration = joblet->FinishTime - joblet->StartTime;
        statistics.AddSample("/time/total", duration.MilliSeconds());
        // XXX(babenko): drop after investigating test flaps
        LOG_DEBUG("Joblet finalized (JobId: %v, TimeTotal: %v)",
            joblet->JobId,
            duration);
    }

    if (jobSummary->PrepareDuration) {
        statistics.AddSample("/time/prepare", jobSummary->PrepareDuration->MilliSeconds());
    }
    if (jobSummary->DownloadDuration) {
        statistics.AddSample("/time/artifacts_download", jobSummary->DownloadDuration->MilliSeconds());
    }
    if (jobSummary->ExecDuration) {
        statistics.AddSample("/time/exec", jobSummary->ExecDuration->MilliSeconds());
    }
    if (joblet->JobProxyMemoryReserveFactor) {
        statistics.AddSample("/job_proxy/memory_reserve_factor_x10000", static_cast<int>(1e4 * *joblet->JobProxyMemoryReserveFactor));
    }
}

void TOperationControllerBase::BuildJobAttributes(
    const TJobInfoPtr& job,
    EJobState state,
    bool outputStatistics,
    TFluentMap fluent) const
{
    static const auto EmptyMapYson = TYsonString("{}");

    fluent
        .Item("job_type").Value(FormatEnum(job->JobType))
        .Item("state").Value(state)
        .Item("address").Value(job->NodeDescriptor.Address)
        .Item("start_time").Value(job->StartTime)
        .Item("account").Value(job->Account)
        .Item("progress").Value(job->Progress)

        // We use Int64 for `stderr_size' to be consistent with
        // compressed_data_size / uncompressed_data_size attributes.
        .Item("stderr_size").Value(i64(job->StderrSize))
        .Item("brief_statistics")
            .Value(job->BriefStatistics)
        .DoIf(outputStatistics, [&] (TFluentMap fluent) {
            fluent.Item("statistics")
                .Value(job->StatisticsYson ? job->StatisticsYson : EmptyMapYson);
        })
        .Item("suspicious").Value(job->Suspicious);
}

void TOperationControllerBase::BuildFinishedJobAttributes(
    const TFinishedJobInfoPtr& job,
    bool outputStatistics,
    TFluentMap fluent) const
{
    BuildJobAttributes(job, job->Summary.State, outputStatistics, fluent);

    const auto& summary = job->Summary;
    fluent
        .Item("finish_time").Value(job->FinishTime)
        .DoIf(summary.State == EJobState::Failed, [&] (TFluentMap fluent) {
            auto error = FromProto<TError>(summary.Result.error());
            fluent.Item("error").Value(error);
        })
        .DoIf(summary.Result.HasExtension(TSchedulerJobResultExt::scheduler_job_result_ext),
            [&] (TFluentMap fluent)
        {
            const auto& schedulerResultExt = summary.Result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);
            fluent.Item("core_infos").Value(schedulerResultExt.core_infos());
        })
        .DoIf(static_cast<bool>(job->InputPaths), [&] (TFluentMap fluent) {
            fluent.Item("input_paths").Value(job->InputPaths);
        });
}

TFluentLogEvent TOperationControllerBase::LogFinishedJobFluently(
    ELogEventType eventType,
    const TJobletPtr& joblet,
    const TJobSummary& jobSummary)
{
    return LogEventFluently(eventType)
        .Item("job_id").Value(joblet->JobId)
        .Item("operation_id").Value(OperationId)
        .Item("start_time").Value(joblet->StartTime)
        .Item("finish_time").Value(joblet->FinishTime)
        .Item("resource_limits").Value(joblet->ResourceLimits)
        .Item("statistics").Value(jobSummary.Statistics)
        .Item("node_address").Value(joblet->NodeDescriptor.Address)
        .Item("job_type").Value(joblet->JobType);
}

IYsonConsumer* TOperationControllerBase::GetEventLogConsumer()
{
    VERIFY_THREAD_AFFINITY_ANY();

    return EventLogConsumer_.get();
}

void TOperationControllerBase::OnChunkFailed(const TChunkId& chunkId)
{
    if (chunkId == NullChunkId) {
        LOG_WARNING("Incompatible unavailable chunk found; deprecated node version");
        return;
    }

    auto it = InputChunkMap.find(chunkId);
    if (it == InputChunkMap.end()) {
        LOG_DEBUG("Intermediate chunk has failed (ChunkId: %v)", chunkId);
        if (!OnIntermediateChunkUnavailable(chunkId)) {
            return;
        }

        IntermediateChunkScraper->Start();
    } else {
        LOG_DEBUG("Input chunk has failed (ChunkId: %v)", chunkId);
        OnInputChunkUnavailable(chunkId, &it->second);
    }
}

void TOperationControllerBase::SafeOnIntermediateChunkLocated(const TChunkId& chunkId, const TChunkReplicaList& replicas, bool missing)
{
    if (missing) {
        // We can unstage intermediate chunks (e.g. in automerge) - just skip them.
        return;
    }

    // Intermediate chunks are always replicated.
    if (IsUnavailable(replicas, NErasure::ECodec::None)) {
        OnIntermediateChunkUnavailable(chunkId);
    } else {
        OnIntermediateChunkAvailable(chunkId, replicas);
    }
}

void TOperationControllerBase::SafeOnInputChunkLocated(const TChunkId& chunkId, const TChunkReplicaList& replicas, bool missing)
{
    if (missing) {
        // We must have locked all the relevant input chunks, but when user transaction is aborted
        // there can be a race between operation completion and chunk scraper.
        OnOperationFailed(TError("Input chunk %v is missing", chunkId));
        return;
    }

    ++ChunkLocatedCallCount;
    if (ChunkLocatedCallCount >= Config->ChunkScraper->MaxChunksPerRequest) {
        ChunkLocatedCallCount = 0;
        LOG_DEBUG("Located another batch of chunks (Count: %v, UnavailableInputChunkCount: %v)",
            Config->ChunkScraper->MaxChunksPerRequest,
            UnavailableInputChunkCount);
    }

    auto it = InputChunkMap.find(chunkId);
    YCHECK(it != InputChunkMap.end());

    auto& descriptor = it->second;
    YCHECK(!descriptor.InputChunks.empty());
    auto& chunkSpec = descriptor.InputChunks.front();
    auto codecId = NErasure::ECodec(chunkSpec->GetErasureCodec());

    if (IsUnavailable(replicas, codecId, CheckParityReplicas())) {
        OnInputChunkUnavailable(chunkId, &descriptor);
    } else {
        OnInputChunkAvailable(chunkId, replicas, &descriptor);
    }
}

void TOperationControllerBase::OnInputChunkAvailable(
    const TChunkId& chunkId,
    const TChunkReplicaList& replicas,
    TInputChunkDescriptor* descriptor)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (descriptor->State != EInputChunkState::Waiting) {
        return;
    }

    LOG_TRACE("Input chunk is available (ChunkId: %v)", chunkId);

    --UnavailableInputChunkCount;
    YCHECK(UnavailableInputChunkCount >= 0);

    if (UnavailableInputChunkCount == 0) {
        InputChunkScraper->Stop();
    }

    // Update replicas in place for all input chunks with current chunkId.
    for (auto& chunkSpec : descriptor->InputChunks) {
        chunkSpec->SetReplicaList(replicas);
    }

    descriptor->State = EInputChunkState::Active;

    for (const auto& inputStripe : descriptor->InputStripes) {
        --inputStripe.Stripe->WaitingChunkCount;
        if (inputStripe.Stripe->WaitingChunkCount > 0)
            continue;

        auto task = inputStripe.Task;
        task->GetChunkPoolInput()->Resume(inputStripe.Cookie);
        if (task->HasInputLocality()) {
            AddTaskLocalityHint(inputStripe.Stripe, task);
        }
        AddTaskPendingHint(task);
    }
}

void TOperationControllerBase::OnInputChunkUnavailable(const TChunkId& chunkId, TInputChunkDescriptor* descriptor)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (descriptor->State != EInputChunkState::Active) {
        return;
    }

    LOG_TRACE("Input chunk is unavailable (ChunkId: %v)", chunkId);

    ++UnavailableInputChunkCount;

    switch (Spec_->UnavailableChunkTactics) {
        case EUnavailableChunkAction::Fail:
            OnOperationFailed(TError("Input chunk %v is unavailable",
                chunkId));
            break;

        case EUnavailableChunkAction::Skip: {
            descriptor->State = EInputChunkState::Skipped;
            for (const auto& inputStripe : descriptor->InputStripes) {
                inputStripe.Stripe->DataSlices.erase(
                    std::remove_if(
                        inputStripe.Stripe->DataSlices.begin(),
                        inputStripe.Stripe->DataSlices.end(),
                        [&] (TInputDataSlicePtr slice) {
                            try {
                                return chunkId == slice->GetSingleUnversionedChunkOrThrow()->ChunkId();
                            } catch (const std::exception& ex) {
                                //FIXME(savrus) allow data slices to be unavailable.
                                THROW_ERROR_EXCEPTION("Dynamic table chunk became unavailable") << ex;
                            }
                        }),
                    inputStripe.Stripe->DataSlices.end());

                // Store information that chunk disappeared in the chunk mapping.
                for (const auto& chunk : descriptor->InputChunks) {
                    inputStripe.Task->GetChunkMapping()->OnChunkDisappeared(chunk);
                }

                AddTaskPendingHint(inputStripe.Task);
            }
            InputChunkScraper->Start();
            break;
        }

        case EUnavailableChunkAction::Wait: {
            descriptor->State = EInputChunkState::Waiting;
            for (const auto& inputStripe : descriptor->InputStripes) {
                if (inputStripe.Stripe->WaitingChunkCount == 0) {
                    inputStripe.Task->GetChunkPoolInput()->Suspend(inputStripe.Cookie);
                }
                ++inputStripe.Stripe->WaitingChunkCount;
            }
            InputChunkScraper->Start();
            break;
        }

        default:
            Y_UNREACHABLE();
    }
}

bool TOperationControllerBase::OnIntermediateChunkUnavailable(const TChunkId& chunkId)
{
    auto it = ChunkOriginMap.find(chunkId);
    YCHECK(it != ChunkOriginMap.end());
    auto& completedJob = it->second;

    // If completedJob->Restartable == false, that means that source pool/task don't support lost jobs
    // and we have to use scraper to find new replicas of intermediate chunks.

    if (!completedJob->Restartable && Spec_->UnavailableChunkTactics == EUnavailableChunkAction::Fail) {
        auto error = TError("Intermediate chunk is unavailable")
            << TErrorAttribute("chunk_id", chunkId);
        OnOperationFailed(error, true);
        return false;
    }

    // If job is replayable, we don't track individual unavailable chunks,
    // since we will regenerate them all anyway.
    if (!completedJob->Restartable &&
        completedJob->UnavailableChunks.insert(chunkId).second)
    {
        ++UnavailableIntermediateChunkCount;
    }

    if (completedJob->Suspended)
        return false;

    LOG_DEBUG("Job is lost (Address: %v, JobId: %v, SourceTask: %v, OutputCookie: %v, InputCookie: %v, UnavailableIntermediateChunkCount: %v)",
        completedJob->NodeDescriptor.Address,
        completedJob->JobId,
        completedJob->SourceTask->GetTitle(),
        completedJob->OutputCookie,
        completedJob->InputCookie,
        UnavailableIntermediateChunkCount);

    completedJob->Suspended = true;
    completedJob->DestinationPool->Suspend(completedJob->InputCookie);

    if (completedJob->Restartable) {
        JobCounter->Lost(1);
        completedJob->SourceTask->GetChunkPoolOutput()->Lost(completedJob->OutputCookie);
        completedJob->SourceTask->OnJobLost(completedJob);
        AddTaskPendingHint(completedJob->SourceTask);
    }

    return true;
}

void TOperationControllerBase::OnIntermediateChunkAvailable(const TChunkId& chunkId, const TChunkReplicaList& replicas)
{
    auto it = ChunkOriginMap.find(chunkId);
    YCHECK(it != ChunkOriginMap.end());
    auto& completedJob = it->second;

    if (completedJob->Restartable || !completedJob->Suspended) {
        // Job will either be restarted or all chunks are fine.
        return;
    }

    if (completedJob->UnavailableChunks.erase(chunkId) == 1) {
        for (auto& dataSlice : completedJob->InputStripe->DataSlices) {
            // Intermediate chunks are always unversioned.
            auto inputChunk = dataSlice->GetSingleUnversionedChunkOrThrow();
            if (inputChunk->ChunkId() == chunkId) {
                inputChunk->SetReplicaList(replicas);
            }
        }
        --UnavailableIntermediateChunkCount;

        YCHECK(UnavailableIntermediateChunkCount > 0 ||
            (UnavailableIntermediateChunkCount == 0 && completedJob->UnavailableChunks.empty()));
        if (completedJob->UnavailableChunks.empty()) {
            LOG_DEBUG("Job result is resumed (JobId: %v, InputCookie: %v, UnavailableIntermediateChunkCount: %v)",
                completedJob->JobId,
                completedJob->InputCookie,
                UnavailableIntermediateChunkCount);

            completedJob->Suspended = false;
            completedJob->DestinationPool->Resume(completedJob->InputCookie);

            // TODO (psushin).
            // Unfortunately we don't know what task we are resuming, so
            // add pending hints for all.
            AddAllTaskPendingHints();
        }
    }
}

bool TOperationControllerBase::AreForeignTablesSupported() const
{
    return false;
}

bool TOperationControllerBase::IsOutputLivePreviewSupported() const
{
    return false;
}

bool TOperationControllerBase::IsIntermediateLivePreviewSupported() const
{
    return false;
}

void TOperationControllerBase::OnTransactionsAborted(const std::vector<TTransactionId>& transactionIds)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    // Check if the user transaction is still alive to determine the exact abort reason.
    bool userTransactionAborted = false;
    if (UserTransaction) {
        auto result = WaitFor(UserTransaction->Ping());
        if (result.FindMatching(NTransactionClient::EErrorCode::NoSuchTransaction)) {
            userTransactionAborted = true;
        }
    }

    if (userTransactionAborted) {
        OnOperationAborted(
            GetUserTransactionAbortedError(UserTransaction->GetId()));
    } else {
        OnOperationFailed(
            GetSchedulerTransactionsAbortedError(transactionIds),
            /* flush */ false);
    }
}

std::vector<ITransactionPtr> TOperationControllerBase::GetTransactions()
{
    std::vector<ITransactionPtr> transactions{
        UserTransaction,
        AsyncTransaction,
        InputTransaction,
        OutputTransaction,
        DebugTransaction
    };
    transactions.erase(
        std::remove_if(transactions.begin(), transactions.end(), [] (const auto& transaction) { return !transaction; }),
        transactions.end());
    return transactions;
}

bool TOperationControllerBase::IsInputDataSizeHistogramSupported() const
{
    return false;
}

void TOperationControllerBase::SafeAbort()
{
    LOG_INFO("Aborting operation controller");

    // NB: Errors ignored since we cannot do anything with it.
    Y_UNUSED(WaitFor(Host->FlushOperationNode()));

    // Skip committing anything if operation controller already tried to commit results.
    if (!CommitFinished) {
        std::vector<TOutputTable*> tables;
        if (StderrTable_ && StderrTable_->IsPrepared()) {
            tables.push_back(&StderrTable_.Get());
        }
        if (CoreTable_ && CoreTable_->IsPrepared()) {
            tables.push_back(&CoreTable_.Get());
        }

        if (!tables.empty()) {
            try {
                StartDebugCompletionTransaction();
                BeginUploadOutputTables(tables);
                AttachOutputChunks(tables);
                EndUploadOutputTables(tables);
                CommitDebugCompletionTransaction();

                if (DebugTransaction) {
                    WaitFor(DebugTransaction->Commit())
                        .ThrowOnError();
                }
            } catch (const std::exception& ex) {
                // Bad luck we can't commit transaction.
                // Such a pity can happen for example if somebody aborted our transaction manually.
                LOG_ERROR(ex, "Failed to commit debug transaction");
                // Intentionally do not wait for abort.
                // Transaction object may be in incorrect state, we need to abort using only transaction id.
                AttachTransaction(DebugTransaction->GetId())->Abort();
            }
        }
    }

    std::vector<TFuture<void>> abortTransactionFutures;
    auto abortTransaction = [&] (const ITransactionPtr& transaction, bool sync = true) {
        if (transaction) {
            // Transaction object may be in incorrect state, we need to abort using only transaction id.
            auto asyncResult = AttachTransaction(transaction->GetId())->Abort();
            if (sync) {
                abortTransactionFutures.push_back(asyncResult);
            }
        }
    };

    // NB: We do not abort input transaction synchronously since
    // it can belong to an unavailable remote cluster.
    // Moreover if input transaction abort failed it does not harm anything.
    abortTransaction(InputTransaction, /* sync */ false);
    abortTransaction(OutputTransaction);
    abortTransaction(AsyncTransaction, /* sync */ false);

    WaitFor(Combine(abortTransactionFutures))
        .ThrowOnError();

    State = EControllerState::Finished;

    LogProgress(/* force */ true);

    LOG_INFO("Operation controller aborted");
}

void TOperationControllerBase::SafeComplete()
{
    OnOperationCompleted(true);
}

void TOperationControllerBase::CheckTimeLimit()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    auto timeLimit = GetTimeLimit();
    if (timeLimit) {
        if (TInstant::Now() - StartTime > *timeLimit) {
            OnOperationTimeLimitExceeded();
        }
    }
}

void TOperationControllerBase::CheckAvailableExecNodes()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (AvailableExecNodesWereObserved_) {
        return;
    }

    if (ShouldSkipSanityCheck()) {
        return;
    }

    bool success = false;
    for (const auto& pair : GetExecNodeDescriptors()) {
        const auto& descriptor = pair.second;
        for (const auto& filter : PoolTreeSchedulingTagFilters_) {
            if (descriptor.CanSchedule(filter)) {
                success = true;
                break;
            }
        }
        if (success) {
            break;
        }
    }

    if (!success) {
        OnOperationFailed(TError("No online nodes match operation scheduling tag filter %Qv",
            Spec_->SchedulingTagFilter.GetFormula()));
    } else {
        AvailableExecNodesWereObserved_ = true;
    }
}

void TOperationControllerBase::AnalyzeTmpfsUsage()
{
    if (!Config->EnableTmpfs) {
        return;
    }

    THashMap<EJobType, i64> maximumUsedTmpfsSizePerJobType;
    THashMap<EJobType, TUserJobSpecPtr> userJobSpecPerJobType;

    for (const auto& task : Tasks) {
        const auto& userJobSpecPtr = task->GetUserJobSpec();
        if (!userJobSpecPtr || !userJobSpecPtr->TmpfsPath || !userJobSpecPtr->TmpfsSize) {
            continue;
        }

        auto maxUsedTmpfsSize = task->GetMaximumUsedTmpfsSize();
        if (!maxUsedTmpfsSize) {
            continue;
        }

        auto jobType = task->GetJobType();

        auto it = maximumUsedTmpfsSizePerJobType.find(jobType);
        if (it == maximumUsedTmpfsSizePerJobType.end()) {
            maximumUsedTmpfsSizePerJobType[jobType] = *maxUsedTmpfsSize;
        } else {
            it->second = std::max(it->second, *maxUsedTmpfsSize);
        }

        userJobSpecPerJobType.insert(std::make_pair(jobType, userJobSpecPtr));
    }

    std::vector<TError> innerErrors;

    double minUnusedSpaceRatio = 1.0 - Config->OperationAlerts->TmpfsAlertMaxUnusedSpaceRatio;

    for (const auto& pair : maximumUsedTmpfsSizePerJobType) {
        const auto& userJobSpecPtr = userJobSpecPerJobType[pair.first];
        auto maxUsedTmpfsSize = pair.second;

        bool minUnusedSpaceThresholdOvercome = userJobSpecPtr->TmpfsSize.Get() - maxUsedTmpfsSize >
            Config->OperationAlerts->TmpfsAlertMinUnusedSpaceThreshold;
        bool minUnusedSpaceRatioViolated = maxUsedTmpfsSize <
            minUnusedSpaceRatio * userJobSpecPtr->TmpfsSize.Get();

        if (minUnusedSpaceThresholdOvercome && minUnusedSpaceRatioViolated) {
            TError error(
                "Jobs of type %Qlv use less than %.1f%% of requested tmpfs size",
                pair.first, minUnusedSpaceRatio * 100.0);
            innerErrors.push_back(error
                << TErrorAttribute("max_used_tmpfs_size", maxUsedTmpfsSize)
                << TErrorAttribute("tmpfs_size", *userJobSpecPtr->TmpfsSize));
        }
    }

    TError error;
    if (!innerErrors.empty()) {
        error = TError(
            "Operation has jobs that use less than %.1f%% of requested tmpfs size; "
            "consider specifying tmpfs size closer to actual usage",
            minUnusedSpaceRatio * 100.0) << innerErrors;
    }

    SetOperationAlert(EOperationAlertType::UnusedTmpfsSpace, error);
}

void TOperationControllerBase::AnalyzeInputStatistics()
{
    TError error;
    if (GetUnavailableInputChunkCount() > 0) {
        error = TError(
            "Some input chunks are not available; "
            "the relevant parts of computation will be suspended");
    }

    SetOperationAlert(EOperationAlertType::LostInputChunks, error);
}

void TOperationControllerBase::AnalyzeIntermediateJobsStatistics()
{
    TError error;
    if (JobCounter->GetLost() > 0) {
        error = TError(
            "Some intermediate outputs were lost and will be regenerated; "
            "operation will take longer than usual");
    }

    SetOperationAlert(EOperationAlertType::LostIntermediateChunks, error);
}

void TOperationControllerBase::AnalyzePartitionHistogram()
{ }

void TOperationControllerBase::AnalyzeAbortedJobs()
{
    auto aggregateTimeForJobState = [&] (EJobState state) {
        i64 sum = 0;
        for (auto type : TEnumTraits<EJobType>::GetDomainValues()) {
            auto value = FindNumericValue(
                JobStatistics,
                Format("/time/total/$/%lv/%lv", state, type));

            if (value) {
                sum += *value;
            }
        }

        return sum;
    };

    i64 completedJobsTime = aggregateTimeForJobState(EJobState::Completed);
    i64 abortedJobsTime = aggregateTimeForJobState(EJobState::Aborted);
    double abortedJobsTimeRatio = 1.0;
    if (completedJobsTime > 0) {
        abortedJobsTimeRatio = 1.0 * abortedJobsTime / completedJobsTime;
    }

    TError error;
    if (abortedJobsTime > Config->OperationAlerts->AbortedJobsAlertMaxAbortedTime &&
        abortedJobsTimeRatio > Config->OperationAlerts->AbortedJobsAlertMaxAbortedTimeRatio)
    {
        error = TError(
            "Aborted jobs time ratio is too high, scheduling is likely to be inefficient; "
            "consider increasing job count to make individual jobs smaller")
                << TErrorAttribute("aborted_jobs_time_ratio", abortedJobsTimeRatio);
    }

    SetOperationAlert(EOperationAlertType::LongAbortedJobs, error);
}

void TOperationControllerBase::AnalyzeJobsIOUsage()
{
    std::vector<TError> innerErrors;

    for (auto jobType : TEnumTraits<EJobType>::GetDomainValues()) {
        auto value = FindNumericValue(
            JobStatistics,
            "/user_job/woodpecker/$/completed/" + FormatEnum(jobType));

        if (value && *value > 0) {
            innerErrors.emplace_back("Detected excessive disk IO in %Qlv jobs", jobType);
        }
    }

    TError error;
    if (!innerErrors.empty()) {
        error = TError("Detected excessive disk IO in jobs; consider optimizing disk usage")
            << innerErrors;
    }

    SetOperationAlert(EOperationAlertType::ExcessiveDiskUsage, error);
}

void TOperationControllerBase::AnalyzeJobsCpuUsage()
{
    static const TVector<TString> allCpuStatistics = {
        "/job_proxy/cpu/system/$/completed/",
        "/job_proxy/cpu/user/$/completed/",
        "/user_job/cpu/system/$/completed/",
        "/user_job/cpu/user/$/completed/"
    };

    THashMap<EJobType, TError> jobTypeToError;
    for (const auto& task : Tasks) {
        auto jobType = task->GetJobType();
        if (jobTypeToError.find(jobType) != jobTypeToError.end()) {
            continue;
        }

        const auto& userJobSpecPtr = task->GetUserJobSpec();
        if (!userJobSpecPtr) {
            continue;
        }

        auto summary = FindSummary(JobStatistics, "/time/exec/$/completed/" + FormatEnum(jobType));
        if (!summary) {
            continue;
        }

        i64 totalExecutionTime = summary->GetSum();
        i64 jobCount = summary->GetCount();
        double cpuLimit = userJobSpecPtr->CpuLimit;
        if (jobCount == 0 || totalExecutionTime == 0 || cpuLimit == 0) {
            continue;
        }

        i64 cpuUsage = 0;
        for (const auto& stat : allCpuStatistics) {
            auto value = FindNumericValue(JobStatistics, stat + FormatEnum(jobType));
            cpuUsage += value ? *value : 0;
        }

        TDuration averageJobDuration = TDuration::MilliSeconds(totalExecutionTime / jobCount);
        TDuration totalExecutionDuration = TDuration::MilliSeconds(totalExecutionTime);
        double cpuRatio = static_cast<double>(cpuUsage) / (totalExecutionTime * cpuLimit);

        if (totalExecutionDuration > Config->OperationAlerts->LowCpuUsageAlertMinExecTime &&
            averageJobDuration > Config->OperationAlerts->LowCpuUsageAlertMinAverageJobTime &&
            cpuRatio < Config->OperationAlerts->LowCpuUsageAlertCpuUsageThreshold)
        {
            auto error = TError("Jobs of type %Qlv use %.2f%% of requested cpu limit", jobType, 100 * cpuRatio)
                << TErrorAttribute("cpu_time", cpuUsage)
                << TErrorAttribute("exec_time", totalExecutionDuration)
                << TErrorAttribute("cpu_limit", cpuLimit);
            YCHECK(jobTypeToError.emplace(jobType, error).second);
        }
    }

    TError error;
    if (!jobTypeToError.empty()) {
        std::vector<TError> innerErrors;
        innerErrors.reserve(jobTypeToError.size());
        for (const auto& pair : jobTypeToError) {
            innerErrors.push_back(pair.second);
        }
        error = TError(
            "Average cpu usage of some of your job types is significantly lower than requested 'cpu_limit'. "
            "Consider decreasing cpu_limit in spec of your operation")
            << innerErrors;
    }

    SetOperationAlert(EOperationAlertType::LowCpuUsage, error);
}

void TOperationControllerBase::AnalyzeJobsDuration()
{
    if (OperationType == EOperationType::RemoteCopy || OperationType == EOperationType::Erase) {
        return;
    }

    auto operationDuration = TInstant::Now() - StartTime;

    std::vector<TError> innerErrors;

    for (auto jobType : GetSupportedJobTypesForJobsDurationAnalyzer()) {
        auto completedJobsSummary = FindSummary(
            JobStatistics,
            "/time/total/$/completed/" + FormatEnum(jobType));

        if (!completedJobsSummary) {
            continue;
        }

        auto maxJobDuration = TDuration::MilliSeconds(completedJobsSummary->GetMax());
        auto completedJobCount = completedJobsSummary->GetCount();
        auto avgJobDuration = TDuration::MilliSeconds(completedJobsSummary->GetSum() / completedJobCount);

        if (completedJobCount > Config->OperationAlerts->ShortJobsAlertMinJobCount &&
            operationDuration > maxJobDuration * 2 &&
            avgJobDuration < Config->OperationAlerts->ShortJobsAlertMinJobDuration &&
            GetDataWeightParameterNameForJob(jobType))
        {
            auto error = TError(
                "Average duration of %Qlv jobs is less than %v seconds, try increasing %v in operation spec",
                jobType,
                Config->OperationAlerts->ShortJobsAlertMinJobDuration.Seconds(),
                GetDataWeightParameterNameForJob(jobType))
                    << TErrorAttribute("average_job_duration", avgJobDuration);

            innerErrors.push_back(error);
        }
    }

    TError error;
    if (!innerErrors.empty()) {
        error = TError("Operation has jobs with duration is less than %v seconds, "
                       "that leads to large overhead costs for scheduling",
                       Config->OperationAlerts->ShortJobsAlertMinJobDuration.Seconds())
            << innerErrors;
    }

    SetOperationAlert(EOperationAlertType::ShortJobsDuration, error);
}

void TOperationControllerBase::AnalyzeOperationDuration()
{
    TError error;
    for (const auto& task : Tasks) {
        if (!task->GetUserJobSpec()) {
            continue;
        }
        i64 completedAndRunning = JobCounter->GetCompletedTotal() + JobCounter->GetRunning();
        if (completedAndRunning == 0) {
            continue;
        }
        i64 pending = JobCounter->GetPending();
        TDuration wallTime = GetInstant() - StartTime;
        TDuration estimatedDuration = (wallTime / completedAndRunning) * pending;

        if (wallTime > Config->OperationAlerts->OperationTooLongAlertMinWallTime &&
            estimatedDuration > Config->OperationAlerts->OperationTooLongAlertEstimateDurationThreshold)
        {
            error = TError(
                "Estimated duration of this operation is about %v days; "
                "consider breaking operation into smaller ones",
                estimatedDuration.Days()) << TErrorAttribute("estimated_duration", estimatedDuration);
            break;
        }
    }
    SetOperationAlert(EOperationAlertType::OperationTooLong, error);
}

void TOperationControllerBase::AnalyzeScheduleJobStatistics()
{
    auto jobSpecThrottlerActivationCount = ScheduleJobStatistics_->Failed[EScheduleJobFailReason::JobSpecThrottling];
    auto activationCountThreshold = Config->OperationAlerts->JobSpecThrottlingAlertActivationCountThreshold;

    TError error;
    if (jobSpecThrottlerActivationCount > activationCountThreshold) {
        error = TError(
            "Excessive job spec throttling is detected. Usage ratio of operation can be "
            "significantly less than fair share ratio")
                << TErrorAttribute("job_spec_throttler_activation_count", jobSpecThrottlerActivationCount);
    }

    SetOperationAlert(EOperationAlertType::ExcessiveJobSpecThrottling, error);
}

void TOperationControllerBase::AnalyzeOperationProgress()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    AnalyzeTmpfsUsage();
    AnalyzeInputStatistics();
    AnalyzeIntermediateJobsStatistics();
    AnalyzePartitionHistogram();
    AnalyzeAbortedJobs();
    AnalyzeJobsIOUsage();
    AnalyzeJobsCpuUsage();
    AnalyzeJobsDuration();
    AnalyzeOperationDuration();
    AnalyzeScheduleJobStatistics();
}

void TOperationControllerBase::UpdateCachedMaxAvailableExecNodeResources()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    const auto& nodeDescriptors = GetExecNodeDescriptors();

    TJobResources maxAvailableResources;
    for (const auto& pair : nodeDescriptors) {
        maxAvailableResources = Max(maxAvailableResources, pair.second.ResourceLimits);
    }

    CachedMaxAvailableExecNodeResources_ = maxAvailableResources;
}

void TOperationControllerBase::CheckMinNeededResourcesSanity()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (ShouldSkipSanityCheck()) {
        return;
    }

    for (const auto& task : Tasks) {
        if (task->GetPendingJobCount() == 0) {
            continue;
        }

        const auto& neededResources = task->GetMinNeededResources().ToJobResources();
        if (!Dominates(*CachedMaxAvailableExecNodeResources_, neededResources)) {
            OnOperationFailed(
                TError("No online node can satisfy the resource demand")
                    << TErrorAttribute("task_id", task->GetTitle())
                    << TErrorAttribute("needed_resources", neededResources)
                    << TErrorAttribute("max_available_resources", *CachedMaxAvailableExecNodeResources_));
        }
    }
}

TScheduleJobResultPtr TOperationControllerBase::SafeScheduleJob(
    ISchedulingContext* context,
    const NScheduler::TJobResourcesWithQuota& jobLimits,
    const TString& treeId)
{
    if (Spec_->TestingOperationOptions->SchedulingDelay) {
        if (Spec_->TestingOperationOptions->SchedulingDelayType == ESchedulingDelayType::Async) {
            TDelayedExecutor::WaitForDuration(*Spec_->TestingOperationOptions->SchedulingDelay);
        } else {
            Sleep(*Spec_->TestingOperationOptions->SchedulingDelay);
        }
    }

    // SafeScheduleJob must be synchronous; context switches are prohibited.
    TForbidContextSwitchGuard contextSwitchGuard;

    TWallTimer timer;
    auto scheduleJobResult = New<TScheduleJobResult>();
    DoScheduleJob(context, jobLimits, treeId, scheduleJobResult.Get());
    if (scheduleJobResult->StartDescriptor) {
        JobCounter->Start(1);
    }
    scheduleJobResult->Duration = timer.GetElapsedTime();

    ScheduleJobStatistics_->RecordJobResult(*scheduleJobResult);

    auto now = NProfiling::GetCpuInstant();
    if (now > ScheduleJobStatisticsLogDeadline_) {
        LOG_DEBUG("Schedule job statistics (Count: %v, TotalDuration: %v, FailureReasons: %v)",
            ScheduleJobStatistics_->Count,
            ScheduleJobStatistics_->Duration,
            ScheduleJobStatistics_->Failed);
        ScheduleJobStatisticsLogDeadline_ = now + NProfiling::DurationToCpuDuration(Config->ScheduleJobStatisticsLogBackoff);
    }

    return scheduleJobResult;
}

void TOperationControllerBase::UpdateConfig(const TControllerAgentConfigPtr& config)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    Config = config;
}

void TOperationControllerBase::CustomizeJoblet(const TJobletPtr& /* joblet */)
{ }

void TOperationControllerBase::CustomizeJobSpec(const TJobletPtr& joblet, TJobSpec* jobSpec) const
{
    auto* schedulerJobSpecExt = jobSpec->MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);

    schedulerJobSpecExt->set_lfalloc_buffer_size(GetLFAllocBufferSize());
    if (OutputTransaction) {
        ToProto(schedulerJobSpecExt->mutable_output_transaction_id(), OutputTransaction->GetId());
    }

    if (joblet->Task->GetUserJobSpec()) {
        InitUserJobSpec(
            schedulerJobSpecExt->mutable_user_job_spec(),
            joblet);
    }
}

void TOperationControllerBase::RegisterTask(TTaskPtr task)
{
    task->Initialize();
    Tasks.emplace_back(std::move(task));
}

void TOperationControllerBase::RegisterTaskGroup(TTaskGroupPtr group)
{
    TaskGroups.push_back(std::move(group));
}

void TOperationControllerBase::UpdateTask(TTaskPtr task)
{
    int oldPendingJobCount = CachedPendingJobCount;
    int newPendingJobCount = CachedPendingJobCount + task->GetPendingJobCountDelta();
    CachedPendingJobCount = newPendingJobCount;

    int oldTotalJobCount = JobCounter->GetTotal();
    JobCounter->Increment(task->GetTotalJobCountDelta());
    int newTotalJobCount = JobCounter->GetTotal();

    IncreaseNeededResources(task->GetTotalNeededResourcesDelta());

    LOG_DEBUG_IF(
        newPendingJobCount != oldPendingJobCount || newTotalJobCount != oldTotalJobCount,
        "Task updated (Task: %v, PendingJobCount: %v -> %v, TotalJobCount: %v -> %v, NeededResources: %v)",
        task->GetTitle(),
        oldPendingJobCount,
        newPendingJobCount,
        oldTotalJobCount,
        newTotalJobCount,
        FormatResources(CachedNeededResources));

    task->CheckCompleted();
}

void TOperationControllerBase::UpdateAllTasks()
{
    for (const auto& task: Tasks) {
        UpdateTask(task);
    }
}

void TOperationControllerBase::UpdateAllTasksIfNeeded()
{
    auto now = NProfiling::GetCpuInstant();
    if (now < TaskUpdateDeadline_) {
        return;
    }
    UpdateAllTasks();
    TaskUpdateDeadline_ = now + NProfiling::DurationToCpuDuration(Config->TaskUpdatePeriod);
}

void TOperationControllerBase::MoveTaskToCandidates(
    TTaskPtr task,
    std::multimap<i64, TTaskPtr>& candidateTasks)
{
    const auto& neededResources = task->GetMinNeededResources();
    i64 minMemory = neededResources.GetMemory();
    candidateTasks.insert(std::make_pair(minMemory, task));
    LOG_DEBUG("Task moved to candidates (Task: %v, MinMemory: %v)",
        task->GetTitle(),
        minMemory / 1_MB);

}

void TOperationControllerBase::AddTaskPendingHint(const TTaskPtr& task)
{
    auto pendingJobCount = task->GetPendingJobCount();
    const auto& taskId = task->GetTitle();
    LOG_TRACE("Adding task pending hint (Task: %v, PendingJobCount: %v)", taskId, pendingJobCount);
    if (pendingJobCount > 0) {
        auto group = task->GetGroup();
        if (group->NonLocalTasks.insert(task).second) {
            LOG_TRACE("Task pending hint added (Task: %v)", taskId);
            MoveTaskToCandidates(task, group->CandidateTasks);
        }
    }
    UpdateTask(task);
}

void TOperationControllerBase::AddAllTaskPendingHints()
{
    for (const auto& task : Tasks) {
        AddTaskPendingHint(task);
    }
}

void TOperationControllerBase::DoAddTaskLocalityHint(TTaskPtr task, TNodeId nodeId)
{
    auto group = task->GetGroup();
    if (group->NodeIdToTasks[nodeId].insert(task).second) {
        LOG_TRACE("Task locality hint added (Task: %v, Address: %v)",
            task->GetTitle(),
            InputNodeDirectory_->GetDescriptor(nodeId).GetDefaultAddress());
    }
}

void TOperationControllerBase::AddTaskLocalityHint(TNodeId nodeId, const TTaskPtr& task)
{
    DoAddTaskLocalityHint(task, nodeId);
    UpdateTask(task);
}

void TOperationControllerBase::AddTaskLocalityHint(const TChunkStripePtr& stripe, const TTaskPtr& task)
{
    for (const auto& dataSlice : stripe->DataSlices) {
        for (const auto& chunkSlice : dataSlice->ChunkSlices) {
            for (auto replica : chunkSlice->GetInputChunk()->GetReplicaList()) {
                auto locality = chunkSlice->GetLocality(replica.GetReplicaIndex());
                if (locality > 0) {
                    DoAddTaskLocalityHint(task, replica.GetNodeId());
                }
            }
        }
    }
    UpdateTask(task);
}

void TOperationControllerBase::ResetTaskLocalityDelays()
{
    LOG_DEBUG("Task locality delays are reset");
    for (auto group : TaskGroups) {
        for (const auto& pair : group->DelayedTasks) {
            auto task = pair.second;
            if (task->GetPendingJobCount() > 0) {
                MoveTaskToCandidates(task, group->CandidateTasks);
            } else {
                LOG_DEBUG("Task pending hint removed (Task: %v)",
                    task->GetTitle());
                YCHECK(group->NonLocalTasks.erase(task) == 1);
            }
        }
        group->DelayedTasks.clear();
    }
}

bool TOperationControllerBase::CheckJobLimits(
    TTaskPtr task,
    const TJobResources& jobLimits,
    const TJobResources& nodeResourceLimits)
{
    auto neededResources = task->GetMinNeededResources().ToJobResources();
    if (Dominates(jobLimits, neededResources)) {
        return true;
    }
    task->CheckResourceDemandSanity(nodeResourceLimits, neededResources);
    return false;
}

void TOperationControllerBase::DoScheduleJob(
    ISchedulingContext* context,
    const NScheduler::TJobResourcesWithQuota& jobLimits,
    const TString& treeId,
    TScheduleJobResult* scheduleJobResult)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (!IsRunning()) {
        LOG_TRACE("Operation is not running, scheduling request ignored");
        scheduleJobResult->RecordFail(EScheduleJobFailReason::OperationNotRunning);
    } else if (GetPendingJobCount() == 0) {
        LOG_TRACE("No pending jobs left, scheduling request ignored");
        scheduleJobResult->RecordFail(EScheduleJobFailReason::NoPendingJobs);
    } else {
        if (!CanSatisfyDiskRequest(context->DiskInfo(), jobLimits.GetDiskQuota())) {
            scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughResources);
            return;
        }
        DoScheduleLocalJob(context, jobLimits.ToJobResources(), treeId, scheduleJobResult);
        if (!scheduleJobResult->StartDescriptor) {
            DoScheduleNonLocalJob(context, jobLimits.ToJobResources(), treeId, scheduleJobResult);
        }
    }
}

void TOperationControllerBase::DoScheduleLocalJob(
    ISchedulingContext* context,
    const TJobResources& jobLimits,
    const TString& treeId,
    TScheduleJobResult* scheduleJobResult)
{
    const auto& nodeResourceLimits = context->ResourceLimits();
    const auto& address = context->GetNodeDescriptor().Address;
    auto nodeId = context->GetNodeDescriptor().Id;

    for (const auto& group : TaskGroups) {
        if (scheduleJobResult->IsScheduleStopNeeded()) {
            return;
        }
        if (!Dominates(jobLimits, group->MinNeededResources)) {
            scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughResources);
            continue;
        }

        auto localTasksIt = group->NodeIdToTasks.find(nodeId);
        if (localTasksIt == group->NodeIdToTasks.end()) {
            continue;
        }

        i64 bestLocality = 0;
        TTaskPtr bestTask;

        auto& localTasks = localTasksIt->second;
        auto it = localTasks.begin();
        while (it != localTasks.end()) {
            auto jt = it++;
            auto task = *jt;

            // Make sure that the task has positive locality.
            // Remove pending hint if not.
            auto locality = task->GetLocality(nodeId);
            if (locality <= 0) {
                localTasks.erase(jt);
                LOG_TRACE("Task locality hint removed (Task: %v, Address: %v)",
                    task->GetTitle(),
                    address);
                continue;
            }

            if (locality <= bestLocality) {
                continue;
            }

            if (task->GetPendingJobCount() == 0) {
                UpdateTask(task);
                continue;
            }

            if (!CheckJobLimits(task, jobLimits, nodeResourceLimits)) {
                scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughResources);
                continue;
            }

            bestLocality = locality;
            bestTask = task;
        }

        if (!IsRunning()) {
            scheduleJobResult->RecordFail(EScheduleJobFailReason::OperationNotRunning);
            break;
        }

        if (bestTask) {
            LOG_DEBUG(
                "Attempting to schedule a local job (Task: %v, Address: %v, Locality: %v, JobLimits: %v, "
                "PendingDataWeight: %v, PendingJobCount: %v)",
                bestTask->GetTitle(),
                address,
                bestLocality,
                FormatResources(jobLimits),
                bestTask->GetPendingDataWeight(),
                bestTask->GetPendingJobCount());

            if (!HasEnoughChunkLists(bestTask->IsStderrTableEnabled(), bestTask->IsCoreTableEnabled())) {
                LOG_DEBUG("Job chunk list demand is not met");
                scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughChunkLists);
                break;
            }

            bestTask->ScheduleJob(context, jobLimits, treeId, IsTreeTentative(treeId), scheduleJobResult);
            if (scheduleJobResult->StartDescriptor) {
                UpdateTask(bestTask);
                break;
            }
            if (scheduleJobResult->IsScheduleStopNeeded()) {
                return;
            }
        } else {
            // NB: This is one of the possible reasons, hopefully the most probable.
            scheduleJobResult->RecordFail(EScheduleJobFailReason::NoLocalJobs);
        }
    }
}

void TOperationControllerBase::DoScheduleNonLocalJob(
    ISchedulingContext* context,
    const TJobResources& jobLimits,
    const TString& treeId,
    TScheduleJobResult* scheduleJobResult)
{
    auto now = NProfiling::CpuInstantToInstant(context->GetNow());
    const auto& nodeResourceLimits = context->ResourceLimits();
    const auto& address = context->GetNodeDescriptor().Address;

    for (const auto& group : TaskGroups) {
        if (scheduleJobResult->IsScheduleStopNeeded()) {
            return;
        }
        if (!Dominates(jobLimits, group->MinNeededResources)) {
            scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughResources);
            continue;
        }

        auto& nonLocalTasks = group->NonLocalTasks;
        auto& candidateTasks = group->CandidateTasks;
        auto& delayedTasks = group->DelayedTasks;

        // Move tasks from delayed to candidates.
        while (!delayedTasks.empty()) {
            auto it = delayedTasks.begin();
            auto deadline = it->first;
            if (now < deadline) {
                break;
            }
            auto task = it->second;
            delayedTasks.erase(it);
            if (task->GetPendingJobCount() == 0) {
                LOG_DEBUG("Task pending hint removed (Task: %v)",
                    task->GetTitle());
                YCHECK(nonLocalTasks.erase(task) == 1);
                UpdateTask(task);
            } else {
                LOG_DEBUG("Task delay deadline reached (Task: %v)", task->GetTitle());
                MoveTaskToCandidates(task, candidateTasks);
            }
        }

        // Consider candidates in the order of increasing memory demand.
        {
            int processedTaskCount = 0;
            int noPendingJobsTaskCount = 0;
            auto it = candidateTasks.begin();
            while (it != candidateTasks.end()) {
                ++processedTaskCount;
                auto task = it->second;

                // Make sure that the task is ready to launch jobs.
                // Remove pending hint if not.
                if (task->GetPendingJobCount() == 0) {
                    LOG_DEBUG("Task pending hint removed (Task: %v)", task->GetTitle());
                    candidateTasks.erase(it++);
                    YCHECK(nonLocalTasks.erase(task) == 1);
                    UpdateTask(task);
                    ++noPendingJobsTaskCount;
                    continue;
                }

                // Check min memory demand for early exit.
                if (task->GetMinNeededResources().GetMemory() > jobLimits.GetMemory()) {
                    scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughResources);
                    break;
                }

                if (!CheckJobLimits(task, jobLimits, nodeResourceLimits)) {
                    ++it;
                    scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughResources);
                    continue;
                }

                if (!task->GetDelayedTime()) {
                    task->SetDelayedTime(now);
                }

                auto deadline = *task->GetDelayedTime() + task->GetLocalityTimeout();
                if (deadline > now) {
                    LOG_DEBUG("Task delayed (Task: %v, Deadline: %v)",
                        task->GetTitle(),
                        deadline);
                    delayedTasks.insert(std::make_pair(deadline, task));
                    candidateTasks.erase(it++);
                    scheduleJobResult->RecordFail(EScheduleJobFailReason::TaskDelayed);
                    continue;
                }

                if (!IsRunning()) {
                    scheduleJobResult->RecordFail(EScheduleJobFailReason::OperationNotRunning);
                    break;
                }

                LOG_DEBUG(
                    "Attempting to schedule a non-local job (Task: %v, Address: %v, JobLimits: %v, "
                    "PendingDataWeight: %v, PendingJobCount: %v)",
                    task->GetTitle(),
                    address,
                    FormatResources(jobLimits),
                    task->GetPendingDataWeight(),
                    task->GetPendingJobCount());

                if (!HasEnoughChunkLists(task->IsStderrTableEnabled(), task->IsCoreTableEnabled())) {
                    LOG_DEBUG("Job chunk list demand is not met");
                    scheduleJobResult->RecordFail(EScheduleJobFailReason::NotEnoughChunkLists);
                    break;
                }

                task->ScheduleJob(context, jobLimits, treeId, IsTreeTentative(treeId), scheduleJobResult);
                if (scheduleJobResult->StartDescriptor) {
                    UpdateTask(task);
                    return;
                }
                if (scheduleJobResult->IsScheduleStopNeeded()) {
                    return;
                }

                // If task failed to schedule job, its min resources might have been updated.
                auto minMemory = task->GetMinNeededResources().GetMemory();
                if (it->first == minMemory) {
                    ++it;
                } else {
                    it = candidateTasks.erase(it);
                    candidateTasks.insert(std::make_pair(minMemory, task));
                }
            }

            if (processedTaskCount == noPendingJobsTaskCount) {
                scheduleJobResult->RecordFail(EScheduleJobFailReason::NoCandidateTasks);
            }

            LOG_DEBUG("Non-local tasks processed (TotalCount: %v, NoPendingJobsCount: %v)",
                processedTaskCount,
                noPendingJobsTaskCount);
        }
    }
}

bool TOperationControllerBase::IsTreeTentative(const TString& treeId) const
{
    return Spec_->TentativePoolTrees.has(treeId);
}

TCancelableContextPtr TOperationControllerBase::GetCancelableContext() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableContext;
}

IInvokerPtr TOperationControllerBase::GetCancelableInvoker() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return CancelableInvoker;
}

IInvokerPtr TOperationControllerBase::GetInvoker() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    return SuspendableInvoker;
}

TFuture<void> TOperationControllerBase::Suspend()
{
    VERIFY_THREAD_AFFINITY_ANY();

    if (Spec_->TestingOperationOptions->DelayInsideSuspend) {
        return Combine(std::vector<TFuture<void>> {
            SuspendableInvoker->Suspend(),
            TDelayedExecutor::MakeDelayed(*Spec_->TestingOperationOptions->DelayInsideSuspend)});
    }

    return SuspendableInvoker->Suspend();
}

void TOperationControllerBase::Resume()
{
    VERIFY_THREAD_AFFINITY_ANY();

    SuspendableInvoker->Resume();
}

void TOperationControllerBase::Cancel()
{
    VERIFY_THREAD_AFFINITY_ANY();

    CancelableContext->Cancel();

    LOG_INFO("Operation controller canceled");
}

int TOperationControllerBase::GetPendingJobCount() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    // Avoid accessing the state while not prepared.
    if (!IsPrepared()) {
        return 0;
    }

    // NB: For suspended operations we still report proper pending job count
    // but zero demand.
    if (!IsRunning()) {
        return 0;
    }

    return CachedPendingJobCount;
}

void TOperationControllerBase::IncreaseNeededResources(const TJobResources& resourcesDelta)
{
    VERIFY_THREAD_AFFINITY_ANY();

    TWriterGuard guard(CachedNeededResourcesLock);
    CachedNeededResources += resourcesDelta;
}

TJobResources TOperationControllerBase::GetNeededResources() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    TReaderGuard guard(CachedNeededResourcesLock);
    return CachedNeededResources;
}

TJobResourcesWithQuotaList TOperationControllerBase::GetMinNeededJobResources() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    NConcurrency::TReaderGuard guard(CachedMinNeededResourcesJobLock);
    return CachedMinNeededJobResources;
}

void TOperationControllerBase::UpdateMinNeededJobResources()
{
    VERIFY_THREAD_AFFINITY_ANY();

    CancelableInvoker->Invoke(BIND([=, this_ = MakeStrong(this)] {
        VERIFY_INVOKER_AFFINITY(CancelableInvoker);

        THashMap<EJobType, NScheduler::TJobResourcesWithQuota> minNeededJobResources;

        for (const auto& task : Tasks) {
            if (task->GetPendingJobCount() == 0) {
                continue;
            }

            auto jobType = task->GetJobType();
            auto resources = task->GetMinNeededResources();

            auto resIt = minNeededJobResources.find(jobType);
            if (resIt == minNeededJobResources.end()) {
                minNeededJobResources[jobType] = resources;
            } else {
                resIt->second = Min(resIt->second, resources);
            }
        }

        TJobResourcesWithQuotaList result;
        for (const auto& pair : minNeededJobResources) {
            result.push_back(pair.second);
            LOG_DEBUG("Aggregated minimal needed resources for jobs (JobType: %v, MinNeededResources: %v)",
                pair.first,
                FormatResources(pair.second));
        }

        {
            NConcurrency::TWriterGuard guard(CachedMinNeededResourcesJobLock);
            CachedMinNeededJobResources.swap(result);
        }
    }));
}

void TOperationControllerBase::FlushOperationNode(bool checkFlushResult)
{
    // Some statistics are reported only on operation end so
    // we need to synchronously check everything and set
    // appropriate alerts before flushing operation node.
    // Flush of newly calculated statistics is guaranteed by OnOperationFailed.
    AnalyzeOperationProgress();

    auto flushResult = WaitFor(Host->FlushOperationNode());
    if (checkFlushResult && !flushResult.IsOK()) {
        // We do not want to complete operation if progress flush has failed.
        OnOperationFailed(flushResult, /* flush */ false);
    }
}

void TOperationControllerBase::OnOperationCompleted(bool interrupted)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);
    Y_UNUSED(interrupted);

    // This can happen if operation failed during completion in derived class (e.g. SortController).
    if (State == EControllerState::Finished) {
        return;
    }
    State = EControllerState::Finished;

    BuildAndSaveProgress();
    FlushOperationNode(/* checkFlushResult */ true);

    LogProgress(/* force */ true);

    Host->OnOperationCompleted();
}

void TOperationControllerBase::OnOperationFailed(const TError& error, bool flush)
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    // During operation failing job aborting can lead to another operation fail, we don't want to invoke it twice.
    if (State == EControllerState::Finished) {
        return;
    }
    State = EControllerState::Finished;

    BuildAndSaveProgress();
    LogProgress(/* force */ true);

    if (flush) {
        // NB: Error ignored since we cannot do anything with it.
        FlushOperationNode(/* checkFlushResult */ false);
    }

    Host->OnOperationFailed(error);
}

void TOperationControllerBase::OnOperationAborted(const TError& error)
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    // Cf. OnOperationFailed.
    if (State == EControllerState::Finished) {
        return;
    }
    State = EControllerState::Finished;

    Host->OnOperationAborted(error);
}

TNullable<TDuration> TOperationControllerBase::GetTimeLimit() const
{
    auto timeLimit = Config->OperationTimeLimit;
    if (Spec_->TimeLimit) {
        timeLimit = Spec_->TimeLimit;
    }
    return timeLimit;
}

TError TOperationControllerBase::GetTimeLimitError() const
{
    return TError("Operation is running for too long, aborted")
        << TErrorAttribute("time_limit", GetTimeLimit());
}

void TOperationControllerBase::OnOperationTimeLimitExceeded()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (State == EControllerState::Running) {
        State = EControllerState::Failing;
    }

    for (const auto& joblet : JobletMap) {
        Host->FailJob(joblet.first);
    }

    auto error = GetTimeLimitError();
    if (!JobletMap.empty()) {
        TDelayedExecutor::MakeDelayed(Config->OperationControllerFailTimeout)
            .Apply(BIND(&TOperationControllerBase::OnOperationFailed, MakeWeak(this), error, /* flush */ true)
            .Via(CancelableInvoker));
    } else {
        OnOperationFailed(error, /* flush */ true);
    }
}

void TOperationControllerBase::CheckFailedJobsStatusReceived()
{
    if (IsFailing() && JobletMap.empty()) {
        auto error = GetTimeLimitError();
        OnOperationFailed(error, /* flush */ true);
    }
}

const std::vector<TEdgeDescriptor>& TOperationControllerBase::GetStandardEdgeDescriptors() const
{
    return StandardEdgeDescriptors_;
}

void TOperationControllerBase::InitializeStandardEdgeDescriptors()
{
    StandardEdgeDescriptors_.resize(Sinks_.size());
    for (int index = 0; index < Sinks_.size(); ++index) {
        StandardEdgeDescriptors_[index] = OutputTables_[index].GetEdgeDescriptorTemplate();
        StandardEdgeDescriptors_[index].DestinationPool = Sinks_[index].get();
        StandardEdgeDescriptors_[index].IsFinalOutput = true;
        StandardEdgeDescriptors_[index].LivePreviewIndex = index;
    }
}

void TOperationControllerBase::AddChunksToUnstageList(std::vector<TInputChunkPtr> chunks)
{
    std::vector<TChunkId> chunkIds;
    for (const auto& chunk : chunks) {
        auto it = LivePreviewChunks_.find(chunk);
        YCHECK(it != LivePreviewChunks_.end());
        auto livePreviewDescriptor = it->second;
        DataFlowGraph_->UnregisterLivePreviewChunk(
            livePreviewDescriptor.VertexDescriptor,
            livePreviewDescriptor.LivePreviewIndex,
            chunk);
        chunkIds.emplace_back(chunk->ChunkId());
        LOG_DEBUG("Releasing intermediate chunk (ChunkId: %v, VertexDescriptor: %v, LivePreviewIndex: %v)",
            chunk->ChunkId(),
            livePreviewDescriptor.VertexDescriptor,
            livePreviewDescriptor.LivePreviewIndex);
        LivePreviewChunks_.erase(it);
    }
    Host->AddChunkTreesToUnstageList(std::move(chunkIds), false /* recursive */);
}

void TOperationControllerBase::ProcessSafeException(const std::exception& ex)
{
    OnOperationFailed(TError("Exception thrown in operation controller that led to operation failure")
        << ex);
}

void TOperationControllerBase::ProcessSafeException(const TAssertionFailedException& ex)
{
    TControllerAgentCounterManager::Get()->IncrementAssertionsFailed(OperationType);

    OnOperationFailed(TError("Operation controller crashed; please file a ticket at YTADMINREQ and attach a link to this operation")
        << TErrorAttribute("failed_condition", ex.GetExpression())
        << TErrorAttribute("stack_trace", ex.GetStackTrace())
        << TErrorAttribute("core_path", ex.GetCorePath())
        << TErrorAttribute("operation_id", OperationId));
}

EJobState TOperationControllerBase::GetStatisticsJobState(const TJobletPtr& joblet, const EJobState& state)
{
    // NB: Completed restarted job is considered as lost in statistics.
    // Actually we have lost previous incarnation of this job, but it was already considered as completed in statistics.
    return (joblet->Restarted && state == EJobState::Completed)
        ? EJobState::Lost
        : state;
}

void TOperationControllerBase::ProcessFinishedJobResult(std::unique_ptr<TJobSummary> summary, bool requestJobNodeCreation)
{
    auto jobId = summary->Id;

    const auto& schedulerResultExt = summary->Result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    auto stderrChunkId = FromProto<TChunkId>(schedulerResultExt.stderr_chunk_id());
    auto failContextChunkId = FromProto<TChunkId>(schedulerResultExt.fail_context_chunk_id());

    auto joblet = GetJoblet(jobId);
    // Job is not actually started.
    if (!joblet->StartTime) {
        return;
    }

    bool shouldCreateJobNode =
        (requestJobNodeCreation && JobNodeCount_ < Config->MaxJobNodesPerOperation) ||
        (stderrChunkId && StderrCount_ < Spec_->MaxStderrCount);

    if (stderrChunkId && shouldCreateJobNode) {
        summary->ArchiveStderr = true;
    }
    if (failContextChunkId && shouldCreateJobNode) {
        summary->ArchiveFailContext = true;
    }

    auto inputPaths = BuildInputPathYson(joblet);
    auto finishedJob = New<TFinishedJobInfo>(joblet, std::move(*summary), std::move(inputPaths));
    // NB: we do not want these values to get into the snapshot as they may be pretty large.
    finishedJob->Summary.StatisticsYson = TYsonString();
    finishedJob->Summary.Statistics.Reset();

    if (finishedJob->Summary.ArchiveJobSpec || finishedJob->Summary.ArchiveStderr || finishedJob->Summary.ArchiveFailContext) {
        FinishedJobs_.insert(std::make_pair(jobId, finishedJob));
    }

    if (!shouldCreateJobNode) {
        if (stderrChunkId) {
            Host->AddChunkTreesToUnstageList({stderrChunkId}, false /* recursive */);
        }
        return;
    }

    auto attributes = BuildYsonStringFluently<EYsonType::MapFragment>()
        .Do([&] (TFluentMap fluent) {
            BuildFinishedJobAttributes(finishedJob, /* outputStatistics */ true, fluent);
        })
        .Finish();

    {
        TCreateJobNodeRequest request;
        request.JobId = jobId;
        request.Attributes = attributes;
        request.StderrChunkId = stderrChunkId;
        request.FailContextChunkId = failContextChunkId;

        Host->CreateJobNode(std::move(request));
    }

    if (stderrChunkId) {
        ++StderrCount_;
    }
    ++JobNodeCount_;
}

bool TOperationControllerBase::IsPrepared() const
{
    return State != EControllerState::Preparing;
}

bool TOperationControllerBase::IsRunning() const
{
    return State == EControllerState::Running;
}

bool TOperationControllerBase::IsFailing() const
{
    return State == EControllerState::Failing;
}

bool TOperationControllerBase::IsFinished() const
{
    return State == EControllerState::Finished;
}

void TOperationControllerBase::CreateLivePreviewTables()
{
    const auto& client = Host->GetClient();
    auto connection = client->GetNativeConnection();

    // NB: use root credentials.
    auto channel = client->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    auto addRequest = [&] (
        const TString& path,
        TCellTag cellTag,
        int replicationFactor,
        NCompression::ECodec compressionCodec,
        const TNullable<TString>& account,
        const TString& key,
        const TYsonString& acl,
        TNullable<TTableSchema> schema)
    {
        auto req = TCypressYPathProxy::Create(path);
        req->set_type(static_cast<int>(EObjectType::Table));
        req->set_ignore_existing(true);

        auto attributes = CreateEphemeralAttributes();
        attributes->Set("replication_factor", replicationFactor);
        // Does this affect anything or is this for viewing only? Should we set the 'media' ('primary_medium') property?
        attributes->Set("compression_codec", compressionCodec);
        if (cellTag == connection->GetPrimaryMasterCellTag()) {
            attributes->Set("external", false);
        } else {
            attributes->Set("external_cell_tag", cellTag);
        }
        attributes->Set("acl", acl);
        attributes->Set("inherit_acl", false);
        if (schema) {
            attributes->Set("schema", *schema);
        }
        if (account) {
            attributes->Set("account", *account);
        }
        ToProto(req->mutable_node_attributes(), *attributes);
        GenerateMutationId(req);
        SetTransactionId(req, AsyncTransaction->GetId());

        batchReq->AddRequest(req, key);
    };

    if (IsOutputLivePreviewSupported()) {
        LOG_INFO("Creating live preview for output tables");

        for (int index = 0; index < OutputTables_.size(); ++index) {
            auto& table = OutputTables_[index];
            auto path = GetNewOperationPath(OperationId) + "/output_" + ToString(index);

            addRequest(
                path,
                table.CellTag,
                table.Options->ReplicationFactor,
                table.Options->CompressionCodec,
                table.Options->Account,
                "create_output",
                table.EffectiveAcl,
                table.TableUploadOptions.TableSchema);
        }
    }

    if (StderrTable_) {
        LOG_INFO("Creating live preview for stderr table");

        auto path = GetNewOperationPath(OperationId) + "/stderr";

        addRequest(
            path,
            StderrTable_->CellTag,
            StderrTable_->Options->ReplicationFactor,
            StderrTable_->Options->CompressionCodec,
            Null /* account */,
            "create_stderr",
            StderrTable_->EffectiveAcl,
            StderrTable_->TableUploadOptions.TableSchema);
    }

    if (IsIntermediateLivePreviewSupported()) {
        LOG_INFO("Creating live preview for intermediate table");

        auto path = GetNewOperationPath(OperationId) + "/intermediate";

        addRequest(
            path,
            IntermediateOutputCellTag,
            1,
            Spec_->IntermediateCompressionCodec,
            Null /* account */,
            "create_intermediate",
            BuildYsonStringFluently()
                .BeginList()
                    .Item().BeginMap()
                        .Item("action").Value("allow")
                        .Item("subjects").BeginList()
                            .Item().Value(AuthenticatedUser)
                            .DoFor(Owners, [] (TFluentList fluent, const TString& owner) {
                                fluent.Item().Value(owner);
                            })
                        .EndList()
                        .Item("permissions").BeginList()
                            .Item().Value("read")
                        .EndList()
                        .Item("account").Value(Spec_->IntermediateDataAccount)
                    .EndMap()
                    .DoFor(Spec_->IntermediateDataAcl->GetChildren(), [] (TFluentList fluent, const INodePtr& node) {
                        fluent.Item().Value(node);
                    })
                    .DoFor(Config->AdditionalIntermediateDataAcl->GetChildren(), [] (TFluentList fluent, const INodePtr& node) {
                        fluent.Item().Value(node);
                    })
                .EndList(),
            Null);
    }

    {
        LOG_INFO("Creating intermediate data access node (IntermediateDataAcl: %v)",
            ConvertToYsonString(Spec_->IntermediateDataAcl, EYsonFormat::Text));

        auto path = GetNewOperationPath(OperationId) + "/intermediate_data_access";

        auto req = TCypressYPathProxy::Create(path);
        req->set_type(static_cast<int>(EObjectType::MapNode));
        req->set_ignore_existing(true);

        auto attributes = CreateEphemeralAttributes();
        attributes->Set("acl", ConvertToYsonString(Spec_->IntermediateDataAcl));
        attributes->Set("inherit_acl", false);

        ToProto(req->mutable_node_attributes(), *attributes);
        GenerateMutationId(req);

        batchReq->AddRequest(req, "create_intermediate_data_access");
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error creating live preview tables");
    const auto& batchRsp = batchRspOrError.Value();

    auto handleResponse = [&] (TLivePreviewTableBase& table, TCypressYPathProxy::TRspCreatePtr rsp) {
        table.LivePreviewTableId = FromProto<NCypressClient::TNodeId>(rsp->node_id());
    };

    if (IsOutputLivePreviewSupported()) {
        auto rspsOrError = batchRsp->GetResponses<TCypressYPathProxy::TRspCreate>("create_output");
        YCHECK(rspsOrError.size() == OutputTables_.size());

        for (int index = 0; index < OutputTables_.size(); ++index) {
            handleResponse(OutputTables_[index], rspsOrError[index].Value());
        }

        LOG_INFO("Live preview for output tables created");
    }

    if (StderrTable_) {
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCreate>("create_stderr");
        handleResponse(*StderrTable_, rsp.Value());

        LOG_INFO("Live preview for stderr table created");
    }

    if (IsIntermediateLivePreviewSupported()) {
        auto rsp = batchRsp->GetResponse<TCypressYPathProxy::TRspCreate>("create_intermediate");
        handleResponse(IntermediateTable, rsp.Value());

        LOG_INFO("Live preview for intermediate table created");
    }
}

void TOperationControllerBase::FetchInputTables()
{
    i64 totalChunkCount = 0;
    i64 totalExtensionSize = 0;

    LOG_INFO("Started fetching input tables");

    TQueryOptions queryOptions;
    queryOptions.VerboseLogging = true;
    queryOptions.RangeExpansionLimit = Config->MaxRangesOnTable;

    // We fetch columnar statistics only for the tables that have column selectors specified.
    struct TFetchRequest
    {
        TInputChunkPtr Chunk;
        std::vector<TString> ColumnNames;
    };
    std::vector<TFetchRequest> fetchRequests;

    for (int tableIndex = 0; tableIndex < static_cast<int>(InputTables.size()); ++tableIndex) {
        auto& table = InputTables[tableIndex];
        auto ranges = table.Path.GetRanges();
        int originalRangeCount = ranges.size();
        if (ranges.empty()) {
            continue;
        }
        bool hasColumnSelectors = table.Path.GetColumns().HasValue();

        if (InputQuery && table.Schema.IsSorted()) {
            auto rangeInferrer = CreateRangeInferrer(
                InputQuery->Query->WhereClause,
                table.Schema,
                table.Schema.GetKeyColumns(),
                Host->GetClient()->GetNativeConnection()->GetColumnEvaluatorCache(),
                BuiltinRangeExtractorMap,
                queryOptions);

            std::vector<TReadRange> inferredRanges;
            for (const auto& range : ranges) {
                auto lower = range.LowerLimit().HasKey() ? range.LowerLimit().GetKey() : MinKey();
                auto upper = range.UpperLimit().HasKey() ? range.UpperLimit().GetKey() : MaxKey();
                auto result = rangeInferrer(TRowRange(lower.Get(), upper.Get()), RowBuffer);
                for (const auto& inferred : result) {
                    auto inferredRange = range;
                    inferredRange.LowerLimit().SetKey(TOwningKey(inferred.first));
                    inferredRange.UpperLimit().SetKey(TOwningKey(inferred.second));
                    inferredRanges.push_back(inferredRange);
                }
            }
            ranges = std::move(inferredRanges);
        }

        if (ranges.size() > Config->MaxRangesOnTable) {
            THROW_ERROR_EXCEPTION(
                "Too many ranges on table: maximum allowed %v, actual %v",
                Config->MaxRangesOnTable,
                ranges.size())
                << TErrorAttribute("table_path", table.Path.GetPath());
        }

        LOG_INFO("Fetching input table (Path: %v, RangeCount: %v, InferredRangeCount: %v, HasColumnSelectors: %v)",
            table.Path.GetPath(),
            originalRangeCount,
            ranges.size(),
            hasColumnSelectors);

        std::vector<NChunkClient::NProto::TChunkSpec> chunkSpecs;
        FetchChunkSpecs(
            InputClient,
            InputNodeDirectory_,
            table.CellTag,
            FromObjectId(table.ObjectId),
            ranges,
            table.ChunkCount,
            Config->MaxChunksPerFetch,
            Config->MaxChunksPerLocateRequest,
            [&] (TChunkOwnerYPathProxy::TReqFetchPtr req) {
                req->set_fetch_all_meta_extensions(false);
                req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
                if (table.IsDynamic || IsBoundaryKeysFetchEnabled()) {
                    req->add_extension_tags(TProtoExtensionTag<TBoundaryKeysExt>::Value);
                }
                // NB: we always fetch parity replicas since
                // erasure reader can repair data on flight.
                req->set_fetch_parity_replicas(true);
                SetTransactionId(req, InputTransaction->GetId());
            },
            Logger,
            &chunkSpecs);

        for (const auto& chunkSpec : chunkSpecs) {
            auto inputChunk = New<TInputChunk>(chunkSpec);
            inputChunk->SetTableIndex(tableIndex);
            inputChunk->SetChunkIndex(totalChunkCount++);

            if (inputChunk->GetRowCount() > 0) {
                // Input chunks may have zero row count in case of unsensible read range with coinciding
                // lower and upper row index. We skip such chunks.
                table.Chunks.emplace_back(inputChunk);
                for (const auto& extension : chunkSpec.chunk_meta().extensions().extensions()) {
                    totalExtensionSize += extension.data().size();
                }
                RegisterInputChunk(table.Chunks.back());
                if (hasColumnSelectors) {
                    fetchRequests.emplace_back(TFetchRequest{inputChunk, *table.Path.GetColumns()});
                }
            }
        }

        LOG_INFO("Input table fetched (Path: %v, ChunkCount: %v)",
            table.Path.GetPath(),
            table.Chunks.size());
    }

    if (!fetchRequests.empty()) {
        LOG_INFO("Fetching columnar statistics for tables with column selectors (ChunkCount: %v)",
            fetchRequests.size());

        auto fetcher = New<TColumnarStatisticsFetcher>(
            Config->Fetcher,
            InputNodeDirectory_,
            CancelableInvoker,
            CreateFetcherChunkScraper(),
            InputClient,
            Logger);
        for (const auto& request : fetchRequests) {
            fetcher->AddChunk(std::move(request.Chunk), std::move(request.ColumnNames));
        }
        WaitFor(fetcher->Fetch())
            .ThrowOnError();
        LOG_INFO("Columnar statistics fetched");
        fetcher->SetColumnSelectivityFactors();
    }

    LOG_INFO("Finished fetching input tables (TotalChunkCount: %v, TotalExtensionSize: %v)",
        totalChunkCount,
        totalExtensionSize);
}

void TOperationControllerBase::RegisterInputChunk(const TInputChunkPtr& inputChunk)
{
    const auto& chunkId = inputChunk->ChunkId();

    // Insert an empty TInputChunkDescriptor if a new chunkId is encountered.
    auto& chunkDescriptor = InputChunkMap[chunkId];
    chunkDescriptor.InputChunks.push_back(inputChunk);

    if (IsUnavailable(inputChunk, CheckParityReplicas())) {
        chunkDescriptor.State = EInputChunkState::Waiting;
    }
}

void TOperationControllerBase::LockInputTables()
{
    //! TODO(ignat): Merge in with lock input files method.
    LOG_INFO("Locking input tables");

    auto channel = InputClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);

    auto batchReq = proxy.ExecuteBatch();

    for (const auto& table : InputTables) {
        auto req = TTableYPathProxy::Lock(table.Path.GetPath());
        req->set_mode(static_cast<int>(ELockMode::Snapshot));
        SetTransactionId(req, InputTransaction->GetId());
        GenerateMutationId(req);
        batchReq->AddRequest(req);
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error locking input tables");

    const auto& batchRsp = batchRspOrError.Value()->GetResponses<TCypressYPathProxy::TRspLock>();
    for (int index = 0; index < InputTables.size(); ++index) {
        auto& table = InputTables[index];
        const auto& path = table.Path.GetPath();
        const auto& rspOrError = batchRsp[index];
        THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Failed to lock input table %Qv", path);
        const auto& rsp = rspOrError.Value();
        table.ObjectId = FromProto<TObjectId>(rsp->node_id());
    }
}

void TOperationControllerBase::GetInputTablesAttributes()
{
    LOG_INFO("Getting input tables attributes");

    GetUserObjectBasicAttributes<TInputTable>(
        InputClient,
        InputTables,
        InputTransaction->GetId(),
        Logger,
        EPermission::Read);

    for (const auto& table : InputTables) {
        if (table.Type != EObjectType::Table) {
            THROW_ERROR_EXCEPTION("Object %v has invalid type: expected %Qlv, actual %Qlv",
                table.Path.GetPath(),
                EObjectType::Table,
                table.Type);
        }
    }

    {
        auto channel = InputClient->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();

        for (const auto& table : InputTables) {
            auto objectIdPath = FromObjectId(table.ObjectId);
            {
                auto req = TTableYPathProxy::Get(objectIdPath + "/@");
                std::vector<TString> attributeKeys{
                    "dynamic",
                    "chunk_count",
                    "retained_timestamp",
                    "schema_mode",
                    "schema",
                    "unflushed_timestamp"
                };
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
                SetTransactionId(req, InputTransaction->GetId());
                batchReq->AddRequest(req, "get_attributes");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting attributes of input tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto lockInRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspLock>("lock");
        auto getInAttributesRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGet>("get_attributes");
        for (int index = 0; index < InputTables.size(); ++index) {
            auto& table = InputTables[index];
            auto path = table.Path.GetPath();
            {
                const auto& rsp = getInAttributesRspsOrError[index].Value();
                auto attributes = ConvertToAttributes(TYsonString(rsp->value()));

                table.IsDynamic = attributes->Get<bool>("dynamic");
                table.Schema = attributes->Get<TTableSchema>("schema");
                table.SchemaMode = attributes->Get<ETableSchemaMode>("schema_mode");
                table.ChunkCount = attributes->Get<int>("chunk_count");

                // Validate that timestamp is correct.
                ValidateDynamicTableTimestamp(table.Path, table.IsDynamic, table.Schema, *attributes);
            }
            LOG_INFO("Input table locked (Path: %v, Schema: %v, ChunkCount: %v)",
                path,
                table.Schema,
                table.ChunkCount);
        }
    }
}

void TOperationControllerBase::GetOutputTablesSchema()
{
    LOG_INFO("Getting output tables schema");

    {
        auto channel = OutputClient->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        for (const auto* table : UpdatingTables) {
            auto objectIdPath = FromObjectId(table->ObjectId);
            {
                auto req = TTableYPathProxy::Get(objectIdPath + "/@");
                std::vector<TString> attributeKeys{
                    "schema_mode",
                    "schema",
                    "optimize_for",
                    "compression_codec",
                    "erasure_codec",
                    "dynamic"
                };
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
                SetTransactionId(req, GetTransactionForOutputTable(*table)->GetId());
                batchReq->AddRequest(req, "get_attributes");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error getting attributes of output tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto getOutAttributesRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGet>("get_attributes");
        for (int index = 0; index < UpdatingTables.size(); ++index) {
            auto* table = UpdatingTables[index];
            const auto& path = table->Path;

            const auto& rsp = getOutAttributesRspsOrError[index].Value();
            auto attributes = ConvertToAttributes(TYsonString(rsp->value()));

            if (attributes->Get<bool>("dynamic")) {
                THROW_ERROR_EXCEPTION("Output to dynamic table is not supported")
                    << TErrorAttribute("table_path", path);
            }

            table->TableUploadOptions = GetTableUploadOptions(
                path,
                *attributes,
                0); // Here we assume zero row count, we will do additional check later.

            // TODO(savrus) I would like to see commit ts here. But as for now, start ts suffices.
            table->Timestamp = GetTransactionForOutputTable(*table)->GetStartTimestamp();

            // NB(psushin): This option must be set before PrepareOutputTables call.
            table->Options->EvaluateComputedColumns = table->TableUploadOptions.TableSchema.HasComputedColumns();

            LOG_DEBUG("Received output table schema (Path: %v, Schema: %v, SchemaMode: %v, LockMode: %v)",
                path,
                table->TableUploadOptions.TableSchema,
                table->TableUploadOptions.SchemaMode,
                table->TableUploadOptions.LockMode);
        }

        if (StderrTable_) {
            StderrTable_->TableUploadOptions.TableSchema = GetStderrBlobTableSchema().ToTableSchema();
            StderrTable_->TableUploadOptions.SchemaMode = ETableSchemaMode::Strong;
            if (StderrTable_->TableUploadOptions.UpdateMode == EUpdateMode::Append) {
                THROW_ERROR_EXCEPTION("Cannot write stderr table in append mode.");
            }
        }

        if (CoreTable_) {
            CoreTable_->TableUploadOptions.TableSchema = GetCoreBlobTableSchema().ToTableSchema();
            CoreTable_->TableUploadOptions.SchemaMode = ETableSchemaMode::Strong;
            if (CoreTable_->TableUploadOptions.UpdateMode == EUpdateMode::Append) {
                THROW_ERROR_EXCEPTION("Cannot write core table in append mode.");
            }
        }
    }
}

void TOperationControllerBase::PrepareInputTables()
{
    if (!AreForeignTablesSupported()) {
        for (const auto& table : InputTables) {
            if (table.IsForeign()) {
                THROW_ERROR_EXCEPTION("Foreign tables are not supported in %Qlv operation", OperationType)
                    << TErrorAttribute("foreign_table", table.GetPath());
            }
        }
    }
}

void TOperationControllerBase::PrepareOutputTables()
{ }

void TOperationControllerBase::LockOutputTablesAndGetAttributes()
{
    LOG_INFO("Locking output tables");

    {
        auto channel = OutputClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        {
            auto batchReq = proxy.ExecuteBatch();
            for (const auto* table : UpdatingTables) {
                auto objectIdPath = FromObjectId(table->ObjectId);
                auto req = TCypressYPathProxy::Lock(objectIdPath);
                SetTransactionId(req, GetTransactionForOutputTable(*table)->GetId());
                GenerateMutationId(req);
                req->set_mode(static_cast<int>(table->TableUploadOptions.LockMode));
                batchReq->AddRequest(req, "lock");
            }
            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(
                GetCumulativeError(batchRspOrError),
                "Error locking output tables");
        }
    }

    LOG_INFO("Getting output tables attributes");

    {
        auto channel = OutputClient->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
        TObjectServiceProxy proxy(channel);
        auto batchReq = proxy.ExecuteBatch();

        for (const auto* table : UpdatingTables) {
            auto objectIdPath = FromObjectId(table->ObjectId);
            {
                auto req = TTableYPathProxy::Get(objectIdPath + "/@");

                std::vector<TString> attributeKeys{
                    "account",
                    "chunk_writer",
                    "effective_acl",
                    "primary_medium",
                    "replication_factor",
                    "row_count",
                    "vital",
                    "enable_skynet_sharing",
                };
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
                SetTransactionId(req, GetTransactionForOutputTable(*table)->GetId());
                batchReq->AddRequest(req, "get_attributes");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(
            GetCumulativeError(batchRspOrError),
            "Error getting attributes of output tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto getOutAttributesRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGet>("get_attributes");
        for (int index = 0; index < UpdatingTables.size(); ++index) {
            auto* table = UpdatingTables[index];
            const auto& path = table->Path.GetPath();
            {
                const auto& rsp = getOutAttributesRspsOrError[index].Value();
                auto attributes = ConvertToAttributes(TYsonString(rsp->value()));

                if (attributes->Get<i64>("row_count") > 0 &&
                    table->TableUploadOptions.TableSchema.IsSorted() &&
                    table->TableUploadOptions.UpdateMode == EUpdateMode::Append)
                {
                    THROW_ERROR_EXCEPTION("Cannot append sorted data to non-empty output table %v",
                        path);
                }

                if (table->TableUploadOptions.TableSchema.IsSorted()) {
                    table->Options->ValidateSorted = true;
                    table->Options->ValidateUniqueKeys = table->TableUploadOptions.TableSchema.GetUniqueKeys();
                } else {
                    table->Options->ValidateSorted = false;
                }

                table->Options->CompressionCodec = table->TableUploadOptions.CompressionCodec;
                table->Options->ErasureCodec = table->TableUploadOptions.ErasureCodec;
                table->Options->ReplicationFactor = attributes->Get<int>("replication_factor");
                table->Options->MediumName = attributes->Get<TString>("primary_medium");
                table->Options->Account = attributes->Get<TString>("account");
                table->Options->ChunksVital = attributes->Get<bool>("vital");
                table->Options->OptimizeFor = table->TableUploadOptions.OptimizeFor;
                table->Options->EnableSkynetSharing = attributes->Get<bool>("enable_skynet_sharing", false);

                // Workaround for YT-5827.
                if (table->TableUploadOptions.TableSchema.Columns().empty() &&
                    table->TableUploadOptions.TableSchema.GetStrict())
                {
                    table->Options->OptimizeFor = EOptimizeFor::Lookup;
                }

                table->EffectiveAcl = attributes->GetYson("effective_acl");
                table->WriterConfig = attributes->FindYson("chunk_writer");
            }
            LOG_INFO("Output table locked (Path: %v, Options: %v, UploadTransactionId: %v)",
                path,
                ConvertToYsonString(table->Options, EYsonFormat::Text).GetData(),
                table->UploadTransactionId);
        }
    }
}

void TOperationControllerBase::BeginUploadOutputTables(const std::vector<TOutputTable*>& updatingTables)
{
    LOG_INFO("Beginning upload for output tables");

    {
        auto channel = OutputClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
        TObjectServiceProxy proxy(channel);

        {
            auto batchReq = proxy.ExecuteBatch();
            for (const auto* table : updatingTables) {
                auto objectIdPath = FromObjectId(table->ObjectId);
                auto req = TTableYPathProxy::BeginUpload(objectIdPath);
                SetTransactionId(req, GetTransactionForOutputTable(*table)->GetId());
                GenerateMutationId(req);
                req->set_update_mode(static_cast<int>(table->TableUploadOptions.UpdateMode));
                req->set_lock_mode(static_cast<int>(table->TableUploadOptions.LockMode));
                req->set_upload_transaction_title(Format("Upload to %v from operation %v",
                    table->Path.GetPath(),
                    OperationId));
                batchReq->AddRequest(req, "begin_upload");
            }
            auto batchRspOrError = WaitFor(batchReq->Invoke());
            THROW_ERROR_EXCEPTION_IF_FAILED(
                GetCumulativeError(batchRspOrError),
                "Error starting upload transactions for output tables");
            const auto& batchRsp = batchRspOrError.Value();

            auto beginUploadRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspBeginUpload>("begin_upload");
            for (int index = 0; index < updatingTables.size(); ++index) {
                auto* table = updatingTables[index];
                const auto& rsp = beginUploadRspsOrError[index].Value();
                table->UploadTransactionId = FromProto<TTransactionId>(rsp->upload_transaction_id());
            }
        }
    }

    THashMap<TCellTag, std::vector<TOutputTable*>> cellTagToTables;
    for (auto* table : updatingTables) {
        cellTagToTables[table->CellTag].push_back(table);
    }

    for (const auto& pair : cellTagToTables) {
        auto cellTag = pair.first;
        const auto& tables = pair.second;

        LOG_INFO("Getting output tables upload parameters (CellTag: %v)", cellTag);

        auto channel = OutputClient->GetMasterChannelOrThrow(
            EMasterChannelKind::Follower,
            cellTag);
        TObjectServiceProxy proxy(channel);

        auto batchReq = proxy.ExecuteBatch();
        for (const auto& table : tables) {
            auto objectIdPath = FromObjectId(table->ObjectId);
            {
                auto req = TTableYPathProxy::GetUploadParams(objectIdPath);
                SetTransactionId(req, table->UploadTransactionId);
                batchReq->AddRequest(req, "get_upload_params");
            }
        }

        auto batchRspOrError = WaitFor(batchReq->Invoke());
        THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting upload parameters of output tables");
        const auto& batchRsp = batchRspOrError.Value();

        auto getUploadParamsRspsOrError = batchRsp->GetResponses<TTableYPathProxy::TRspGetUploadParams>("get_upload_params");
        for (int index = 0; index < tables.size(); ++index) {
            auto* table = tables[index];
            const auto& path = table->Path.GetPath();
            {
                const auto& rspOrError = getUploadParamsRspsOrError[index];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting upload parameters of output table %v",
                    path);

                const auto& rsp = rspOrError.Value();
                table->OutputChunkListId = FromProto<TChunkListId>(rsp->chunk_list_id());

                LOG_INFO("Upload parameters of output table received (Path: %v, ChunkListId: %v)",
                    path,
                    table->OutputChunkListId);
            }
        }
    }
}

void TOperationControllerBase::DoFetchUserFiles(const TUserJobSpecPtr& userJobSpec, std::vector<TUserFile>& files)
{
    auto Logger = TLogger(this->Logger)
        .AddTag("TaskTitle: %v", userJobSpec->TaskTitle);
    for (auto& file : files) {
        auto objectIdPath = FromObjectId(file.ObjectId);
        const auto& path = file.Path.GetPath();

        LOG_INFO("Fetching user file (Path: %v)",
            path);

        switch (file.Type) {
            case EObjectType::Table:
                FetchChunkSpecs(
                    InputClient,
                    InputNodeDirectory_,
                    file.CellTag,
                    objectIdPath,
                    file.Path.GetRanges(),
                    file.ChunkCount,
                    Config->MaxChunksPerFetch,
                    Config->MaxChunksPerLocateRequest,
                    [&] (TChunkOwnerYPathProxy::TReqFetchPtr req) {
                        req->set_fetch_all_meta_extensions(false);
                        req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
                        if (file.IsDynamic || IsBoundaryKeysFetchEnabled()) {
                            req->add_extension_tags(TProtoExtensionTag<TBoundaryKeysExt>::Value);
                        }
                        // NB: we always fetch parity replicas since
                        // erasure reader can repair data on flight.
                        req->set_fetch_parity_replicas(true);
                        SetTransactionId(req, InputTransaction->GetId());
                    },
                    Logger,
                    &file.ChunkSpecs);
                break;

            case EObjectType::File: {
                auto channel = InputClient->GetMasterChannelOrThrow(
                    EMasterChannelKind::Follower,
                    file.CellTag);
                TObjectServiceProxy proxy(channel);

                auto batchReq = proxy.ExecuteBatch();

                auto req = TChunkOwnerYPathProxy::Fetch(objectIdPath);
                ToProto(req->mutable_ranges(), std::vector<TReadRange>({TReadRange()}));
                req->add_extension_tags(TProtoExtensionTag<NChunkClient::NProto::TMiscExt>::Value);
                SetTransactionId(req, InputTransaction->GetId());
                batchReq->AddRequest(req, "fetch");

                auto batchRspOrError = WaitFor(batchReq->Invoke());
                THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error fetching user file %v",
                     path);
                const auto& batchRsp = batchRspOrError.Value();

                auto rsp = batchRsp->GetResponse<TChunkOwnerYPathProxy::TRspFetch>("fetch").Value();
                ProcessFetchResponse(
                    InputClient,
                    rsp,
                    file.CellTag,
                    nullptr,
                    Config->MaxChunksPerLocateRequest,
                    Null,
                    Logger,
                    &file.ChunkSpecs);

                break;
            }

            default:
                Y_UNREACHABLE();
        }

        LOG_INFO("User file fetched (Path: %v, FileName: %v)",
            path,
            file.FileName);
    }
}

void TOperationControllerBase::FetchUserFiles()
{
    for (auto& pair : UserJobFiles_) {
        const auto& userJobSpec = pair.first;
        auto& files = pair.second;
        try {
            DoFetchUserFiles(userJobSpec, files);
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error fetching user files")
                << TErrorAttribute("task_title", userJobSpec->TaskTitle)
                << ex;
        }
    }
}

void TOperationControllerBase::LockUserFiles()
{
    LOG_INFO("Locking user files");

    auto channel = OutputClient->GetMasterChannelOrThrow(EMasterChannelKind::Leader);
    TObjectServiceProxy proxy(channel);
    auto batchReq = proxy.ExecuteBatch();

    for (const auto& files : GetValues(UserJobFiles_)) {
        for (const auto& file : files) {
            auto req = TCypressYPathProxy::Lock(file.Path.GetPath());
            req->set_mode(static_cast<int>(ELockMode::Snapshot));
            GenerateMutationId(req);
            SetTransactionId(req, InputTransaction->GetId());
            batchReq->AddRequest(req);
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(GetCumulativeError(batchRspOrError), "Error locking user files");

    const auto& batchRsp = batchRspOrError.Value()->GetResponses<TCypressYPathProxy::TRspLock>();
    int index = 0;
    for (auto& pair : UserJobFiles_) {
        const auto& userJobSpec = pair.first;
        auto& files = pair.second;
        try {
            for (auto& file : files) {
                const auto& path = file.Path.GetPath();
                const auto& rspOrError = batchRsp[index++];
                THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Failed to lock user file %Qv", path);
                const auto& rsp = rspOrError.Value();
                file.ObjectId = FromProto<TObjectId>(rsp->node_id());
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error locking user files")
                 << TErrorAttribute("task_title", userJobSpec->TaskTitle)
                 << ex;
        }
    }
}

void TOperationControllerBase::GetUserFilesAttributes()
{
    LOG_INFO("Getting user files attributes");

    for (auto& pair : UserJobFiles_) {
        const auto& userJobSpec = pair.first;
        auto& files = pair.second;
        GetUserObjectBasicAttributes<TUserFile>(
            Client,
            files,
            InputTransaction->GetId(),
            TLogger(Logger).AddTag("TaskTitle: %v", userJobSpec->TaskTitle),
            EPermission::Read);
    }

    for (const auto& files : GetValues(UserJobFiles_)) {
        for (const auto& file : files) {
            const auto& path = file.Path.GetPath();
            if (!file.IsLayer && file.Type != EObjectType::Table && file.Type != EObjectType::File) {
                THROW_ERROR_EXCEPTION("User file %v has invalid type: expected %Qlv or %Qlv, actual %Qlv",
                    path,
                    EObjectType::Table,
                    EObjectType::File,
                    file.Type);
            } else if (file.IsLayer && file.Type != EObjectType::File) {
                THROW_ERROR_EXCEPTION("User layer %v has invalid type: expected %Qlv , actual %Qlv",
                    path,
                    EObjectType::File,
                    file.Type);
            }
        }
    }


    auto channel = OutputClient->GetMasterChannelOrThrow(EMasterChannelKind::Follower);
    TObjectServiceProxy proxy(channel);
    auto batchReq = proxy.ExecuteBatch();

    for (const auto& files : GetValues(UserJobFiles_)) {
        for (const auto& file : files) {
            auto objectIdPath = FromObjectId(file.ObjectId);
            {
                auto req = TYPathProxy::Get(objectIdPath + "/@");
                SetTransactionId(req, InputTransaction->GetId());
                std::vector<TString> attributeKeys;
                attributeKeys.push_back("file_name");
                switch (file.Type) {
                    case EObjectType::File:
                        attributeKeys.push_back("executable");
                        break;

                    case EObjectType::Table:
                        attributeKeys.push_back("format");
                        attributeKeys.push_back("dynamic");
                        attributeKeys.push_back("schema");
                        attributeKeys.push_back("retained_timestamp");
                        attributeKeys.push_back("unflushed_timestamp");
                        break;

                    default:
                        Y_UNREACHABLE();
                }
                attributeKeys.push_back("key");
                attributeKeys.push_back("chunk_count");
                attributeKeys.push_back("uncompressed_data_size");
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
                batchReq->AddRequest(req, "get_attributes");
            }

            {
                auto req = TYPathProxy::Get(file.Path.GetPath() + "&/@");
                SetTransactionId(req, InputTransaction->GetId());
                std::vector<TString> attributeKeys;
                attributeKeys.push_back("key");
                attributeKeys.push_back("file_name");
                ToProto(req->mutable_attributes()->mutable_keys(), attributeKeys);
                batchReq->AddRequest(req, "get_link_attributes");
            }
        }
    }

    auto batchRspOrError = WaitFor(batchReq->Invoke());
    THROW_ERROR_EXCEPTION_IF_FAILED(batchRspOrError, "Error getting attributes of user files");
    const auto& batchRsp = batchRspOrError.Value();

    auto getAttributesRspsOrError = batchRsp->GetResponses<TYPathProxy::TRspGetKey>("get_attributes");
    auto getLinkAttributesRspsOrError = batchRsp->GetResponses<TYPathProxy::TRspGetKey>("get_link_attributes");

    int index = 0;
    for (auto& pair : UserJobFiles_) {
        const auto& userJobSpec = pair.first;
        auto& files = pair.second;
        THashSet<TString> userFileNames;
        try {
            for (auto& file : files) {
                const auto& path = file.Path.GetPath();

                {
                    const auto& rspOrError = getAttributesRspsOrError[index];
                    THROW_ERROR_EXCEPTION_IF_FAILED(rspOrError, "Error getting attributes of user file %Qv", path);
                    const auto& rsp = rspOrError.Value();
                    const auto& linkRsp = getLinkAttributesRspsOrError[index];
                    index++;

                    file.Attributes = ConvertToAttributes(TYsonString(rsp->value()));
                    const auto& attributes = *file.Attributes;

                    try {
                        if (linkRsp.IsOK()) {
                            auto linkAttributes = ConvertToAttributes(TYsonString(linkRsp.Value()->value()));
                            file.FileName = linkAttributes->Get<TString>("key");
                            file.FileName = linkAttributes->Find<TString>("file_name").Get(file.FileName);
                        } else {
                            file.FileName = attributes.Get<TString>("key");
                            file.FileName = attributes.Find<TString>("file_name").Get(file.FileName);
                        }
                        file.FileName = file.Path.GetFileName().Get(file.FileName);
                    } catch (const std::exception& ex) {
                        // NB: Some of the above Gets and Finds may throw due to, e.g., type mismatch.
                        THROW_ERROR_EXCEPTION("Error parsing attributes of user file %v",
                            path) << ex;
                    }

                    switch (file.Type) {
                        case EObjectType::File:
                            file.Executable = attributes.Get<bool>("executable", false);
                            file.Executable = file.Path.GetExecutable().Get(file.Executable);
                            break;

                        case EObjectType::Table:
                            file.IsDynamic = attributes.Get<bool>("dynamic");
                            file.Schema = attributes.Get<TTableSchema>("schema");
                            file.Format = attributes.FindYson("format");
                            if (!file.Format) {
                                file.Format = file.Path.GetFormat();
                            }
                            // Validate that format is correct.
                            try {
                                if (!file.Format) {
                                    THROW_ERROR_EXCEPTION("Format is missing");
                                }
                                ConvertTo<TFormat>(file.Format);
                            } catch (const std::exception& ex) {
                                THROW_ERROR_EXCEPTION("Failed to parse format of table file %v",
                                    file.Path) << ex;
                            }
                            // Validate that timestamp is correct.
                            ValidateDynamicTableTimestamp(file.Path, file.IsDynamic, file.Schema, attributes);

                            break;

                        default:
                            Y_UNREACHABLE();
                    }

                    i64 fileSize = attributes.Get<i64>("uncompressed_data_size");
                    if (fileSize > Config->MaxFileSize) {
                        THROW_ERROR_EXCEPTION(
                            "User file %v exceeds size limit: %v > %v",
                            path,
                            fileSize,
                            Config->MaxFileSize);
                    }

                    i64 chunkCount = attributes.Get<i64>("chunk_count");
                    if (chunkCount > Config->MaxChunksPerFetch) {
                        THROW_ERROR_EXCEPTION(
                            "User file %v exceeds chunk count limit: %v > %v",
                            path,
                            chunkCount,
                            Config->MaxChunksPerFetch);
                    }
                    file.ChunkCount = chunkCount;

                    LOG_INFO("User file locked (Path: %v, TaskTitle: %v, FileName: %v)",
                        path,
                        userJobSpec->TaskTitle,
                        file.FileName);
                }

                if (!file.IsLayer) {
                    // TODO(babenko): more sanity checks?
                    auto path = file.Path.GetPath();
                    const auto& fileName = file.FileName;

                    if (fileName.empty()) {
                        THROW_ERROR_EXCEPTION("Empty user file name for %v", path);
                    }

                    if (!NFS::CheckPathIsRelativeAndGoesInside(fileName)) {
                        THROW_ERROR_EXCEPTION("User file name cannot reference outside of sandbox directory")
                            << TErrorAttribute("file_name", fileName);
                    }

                    if (!userFileNames.insert(fileName).second) {
                        THROW_ERROR_EXCEPTION("Duplicate user file name %Qv for %v", fileName, path);
                    }
                }
            }
        } catch (const std::exception& ex) {
            THROW_ERROR_EXCEPTION("Error getting user file attributes")
                << TErrorAttribute("task_title", userJobSpec->TaskTitle)
                << ex;
        }
    }
}

void TOperationControllerBase::PrepareInputQuery()
{ }

void TOperationControllerBase::ParseInputQuery(
    const TString& queryString,
    const TNullable<TTableSchema>& schema)
{
    for (const auto& table : InputTables) {
        if (table.Path.GetColumns()) {
            THROW_ERROR_EXCEPTION("Column filter and QL filter cannot appear in the same operation");
        }
    }

    auto externalCGInfo = New<TExternalCGInfo>();
    auto nodeDirectory = New<NNodeTrackerClient::TNodeDirectory>();
    auto fetchFunctions = [&] (const std::vector<TString>& names, const TTypeInferrerMapPtr& typeInferrers) {
        MergeFrom(typeInferrers.Get(), *BuiltinTypeInferrersMap);

        std::vector<TString> externalNames;
        for (const auto& name : names) {
            auto found = typeInferrers->find(name);
            if (found == typeInferrers->end()) {
                externalNames.push_back(name);
            }
        }

        if (externalNames.empty()) {
            return;
        }

        if (!Config->UdfRegistryPath) {
            THROW_ERROR_EXCEPTION("External UDF registry is not configured");
        }

        auto descriptors = LookupAllUdfDescriptors(externalNames, Config->UdfRegistryPath.Get(), Host->GetClient());

        AppendUdfDescriptors(typeInferrers, externalCGInfo, externalNames, descriptors);
    };

    auto inferSchema = [&] () {
        std::vector<TTableSchema> schemas;
        for (const auto& table : InputTables) {
            schemas.push_back(table.Schema);
        }
        return InferInputSchema(schemas, false);
    };

    auto query = PrepareJobQuery(
        queryString,
        schema ? *schema : inferSchema(),
        fetchFunctions);

    auto getColumns = [] (const TTableSchema& desiredSchema, const TTableSchema& tableSchema) {
        std::vector<TString> columns;
        for (const auto& column : desiredSchema.Columns()) {
            if (tableSchema.FindColumn(column.Name())) {
                columns.push_back(column.Name());
            }
        }

        return columns.size() == tableSchema.GetColumnCount()
            ? TNullable<std::vector<TString>>()
            : MakeNullable(std::move(columns));
    };

    // Use query column filter for input tables.
    for (auto table : InputTables) {
        auto columns = getColumns(query->GetReadSchema(), table.Schema);
        if (columns) {
            table.Path.SetColumns(*columns);
        }
    }

    InputQuery.Emplace();
    InputQuery->Query = std::move(query);
    InputQuery->ExternalCGInfo = std::move(externalCGInfo);
}

void TOperationControllerBase::WriteInputQueryToJobSpec(
    NScheduler::NProto::TSchedulerJobSpecExt* schedulerJobSpecExt)
{
    auto* querySpec = schedulerJobSpecExt->mutable_input_query_spec();
    ToProto(querySpec->mutable_query(), InputQuery->Query);
    querySpec->mutable_query()->set_input_row_limit(std::numeric_limits<i64>::max());
    querySpec->mutable_query()->set_output_row_limit(std::numeric_limits<i64>::max());
    ToProto(querySpec->mutable_external_functions(), InputQuery->ExternalCGInfo->Functions);
}

void TOperationControllerBase::CollectTotals()
{
    // This is the sum across all input chunks not accounting lower/upper read limits.
    // Used to calculate compression ratio.
    i64 totalInputDataWeight = 0;
    for (const auto& table : InputTables) {
        for (const auto& inputChunk : table.Chunks) {
            if (IsUnavailable(inputChunk, CheckParityReplicas())) {
                const auto& chunkId = inputChunk->ChunkId();

                switch (Spec_->UnavailableChunkStrategy) {
                    case EUnavailableChunkAction::Fail:
                        THROW_ERROR_EXCEPTION("Input chunk %v is unavailable",
                            chunkId);

                    case EUnavailableChunkAction::Skip:
                        LOG_TRACE("Skipping unavailable chunk (ChunkId: %v)",
                            chunkId);
                        continue;

                    case EUnavailableChunkAction::Wait:
                        // Do nothing.
                        break;

                    default:
                        Y_UNREACHABLE();
                }
            }

            if (table.IsPrimary()) {
                PrimaryInputDataWeight += inputChunk->GetDataWeight();
            } else {
                ForeignInputDataWeight += inputChunk->GetDataWeight();
            }

            totalInputDataWeight += inputChunk->GetTotalDataWeight();
            TotalEstimatedInputUncompressedDataSize += inputChunk->GetUncompressedDataSize();
            TotalEstimatedInputRowCount += inputChunk->GetRowCount();
            TotalEstimatedInputCompressedDataSize += inputChunk->GetCompressedDataSize();
            TotalEstimatedInputDataWeight += inputChunk->GetDataWeight();
            ++TotalEstimatedInputChunkCount;
        }
    }

    InputCompressionRatio = static_cast<double>(TotalEstimatedInputCompressedDataSize) / totalInputDataWeight;
    DataWeightRatio = static_cast<double>(totalInputDataWeight) / TotalEstimatedInputUncompressedDataSize;

    LOG_INFO("Estimated input totals collected (ChunkCount: %v, RowCount: %v, UncompressedDataSize: %v, CompressedDataSize: %v, DataWeight: %v, TotalDataWeight: %v)",
        TotalEstimatedInputChunkCount,
        TotalEstimatedInputRowCount,
        TotalEstimatedInputUncompressedDataSize,
        TotalEstimatedInputCompressedDataSize,
        TotalEstimatedInputDataWeight,
        totalInputDataWeight);
}

void TOperationControllerBase::CustomPrepare()
{ }

void TOperationControllerBase::FillPrepareResult(TOperationControllerPrepareResult* result)
{
    result->Attributes = BuildYsonStringFluently<EYsonType::MapFragment>()
        .Do(BIND(&TOperationControllerBase::BuildPrepareAttributes, Unretained(this)))
        .Finish();
}

void TOperationControllerBase::ClearInputChunkBoundaryKeys()
{
    for (auto& pair : InputChunkMap) {
        auto& inputChunkDescriptor = pair.second;
        for (const auto& chunkSpec : inputChunkDescriptor.InputChunks) {
            // We don't need boundary key ext after preparation phase (for primary tables only).
            if (InputTables[chunkSpec->GetTableIndex()].IsPrimary()) {
                chunkSpec->ReleaseBoundaryKeys();
            }
        }
    }
}

// NB: must preserve order of chunks in the input tables, no shuffling.
std::vector<TInputChunkPtr> TOperationControllerBase::CollectPrimaryChunks(bool versioned) const
{
    std::vector<TInputChunkPtr> result;
    for (const auto& table : InputTables) {
        if (!table.IsForeign() && ((table.IsDynamic && table.Schema.IsSorted()) == versioned)) {
            for (const auto& chunk : table.Chunks) {
                if (IsUnavailable(chunk, CheckParityReplicas())) {
                    switch (Spec_->UnavailableChunkStrategy) {
                        case EUnavailableChunkAction::Skip:
                            continue;

                        case EUnavailableChunkAction::Wait:
                            // Do nothing.
                            break;

                        default:
                            Y_UNREACHABLE();
                    }
                }
                result.push_back(chunk);
            }
        }
    }
    return result;
}

std::vector<TInputChunkPtr> TOperationControllerBase::CollectPrimaryUnversionedChunks() const
{
    return CollectPrimaryChunks(false);
}

std::vector<TInputChunkPtr> TOperationControllerBase::CollectPrimaryVersionedChunks() const
{
    return CollectPrimaryChunks(true);
}

std::pair<i64, i64> TOperationControllerBase::CalculatePrimaryVersionedChunksStatistics() const
{
    i64 dataWeight = 0;
    i64 rowCount = 0;
    for (const auto& table : InputTables) {
        if (!table.IsForeign() && table.IsDynamic && table.Schema.IsSorted()) {
            for (const auto& chunk : table.Chunks) {
                dataWeight += chunk->GetDataWeight();
                rowCount += chunk->GetRowCount();
            }
        }
    }
    return std::make_pair(dataWeight, rowCount);
}

std::vector<TInputDataSlicePtr> TOperationControllerBase::CollectPrimaryVersionedDataSlices(i64 sliceSize)
{
    auto createScraperForFetcher = [&] () -> IFetcherChunkScraperPtr {
        if (Spec_->UnavailableChunkStrategy == EUnavailableChunkAction::Wait) {
            auto scraper = CreateFetcherChunkScraper();
            DataSliceFetcherChunkScrapers.push_back(scraper);
            return scraper;
        } else {
            return IFetcherChunkScraperPtr();
        }
    };

    std::vector<TFuture<void>> asyncResults;
    std::vector<TDataSliceFetcherPtr> fetchers;

    for (const auto& table : InputTables) {
        if (!table.IsForeign() && table.IsDynamic && table.Schema.IsSorted()) {
            auto fetcher = New<TDataSliceFetcher>(
                Config->Fetcher,
                sliceSize,
                table.Schema.GetKeyColumns(),
                true,
                InputNodeDirectory_,
                GetCancelableInvoker(),
                createScraperForFetcher(),
                Host->GetClient(),
                RowBuffer,
                Logger);

            for (const auto& chunk : table.Chunks) {
                if (IsUnavailable(chunk, CheckParityReplicas()) &&
                    Spec_->UnavailableChunkStrategy == EUnavailableChunkAction::Skip)
                {
                    continue;
                }

                fetcher->AddChunk(chunk);
            }

            asyncResults.emplace_back(fetcher->Fetch());
            fetchers.emplace_back(std::move(fetcher));
        }
    }

    WaitFor(Combine(asyncResults))
        .ThrowOnError();

    std::vector<TInputDataSlicePtr> result;
    for (const auto& fetcher : fetchers) {
        for (auto& dataSlice : fetcher->GetDataSlices()) {
            LOG_TRACE("Added dynamic table slice (TablePath: %v, Range: %v..%v, ChunkIds: %v)",
                InputTables[dataSlice->GetTableIndex()].Path.GetPath(),
                dataSlice->LowerLimit(),
                dataSlice->UpperLimit(),
                dataSlice->ChunkSlices);
            result.emplace_back(std::move(dataSlice));
        }
    }

    DataSliceFetcherChunkScrapers.clear();

    return result;
}

std::vector<TInputDataSlicePtr> TOperationControllerBase::CollectPrimaryInputDataSlices(i64 versionedSliceSize)
{
    std::vector<std::vector<TInputDataSlicePtr>> dataSlicesByTableIndex(InputTables.size());
    for (const auto& chunk : CollectPrimaryUnversionedChunks()) {
        auto dataSlice = CreateUnversionedInputDataSlice(CreateInputChunkSlice(chunk));
        dataSlicesByTableIndex[dataSlice->GetTableIndex()].emplace_back(std::move(dataSlice));
    }
    for (auto& dataSlice : CollectPrimaryVersionedDataSlices(versionedSliceSize)) {
        dataSlicesByTableIndex[dataSlice->GetTableIndex()].emplace_back(std::move(dataSlice));
    }
    std::vector<TInputDataSlicePtr> dataSlices;
    for (auto& tableDataSlices : dataSlicesByTableIndex) {
        std::move(tableDataSlices.begin(), tableDataSlices.end(), std::back_inserter(dataSlices));
    }
    return dataSlices;
}

std::vector<std::deque<TInputDataSlicePtr>> TOperationControllerBase::CollectForeignInputDataSlices(int foreignKeyColumnCount) const
{
    std::vector<std::deque<TInputDataSlicePtr>> result;
    for (const auto& table : InputTables) {
        if (table.IsForeign()) {
            result.push_back(std::deque<TInputDataSlicePtr>());

            if (table.IsDynamic && table.Schema.IsSorted()) {
                std::vector<TInputChunkSlicePtr> chunkSlices;
                chunkSlices.reserve(table.Chunks.size());
                for (const auto& chunkSpec : table.Chunks) {
                    chunkSlices.push_back(CreateInputChunkSlice(
                        chunkSpec,
                        RowBuffer->Capture(chunkSpec->BoundaryKeys()->MinKey.Get()),
                        GetKeySuccessor(chunkSpec->BoundaryKeys()->MaxKey.Get(), RowBuffer)));
                }

                auto dataSlices = CombineVersionedChunkSlices(chunkSlices);
                for (const auto& dataSlice : dataSlices) {
                    if (IsUnavailable(dataSlice, CheckParityReplicas())) {
                        switch (Spec_->UnavailableChunkStrategy) {
                            case EUnavailableChunkAction::Skip:
                                continue;

                            case EUnavailableChunkAction::Wait:
                                // Do nothing.
                                break;

                            default:
                                Y_UNREACHABLE();
                        }
                    }
                    result.back().push_back(dataSlice);
                }
            } else {
                for (const auto& inputChunk : table.Chunks) {
                    if (IsUnavailable(inputChunk, CheckParityReplicas())) {
                        switch (Spec_->UnavailableChunkStrategy) {
                            case EUnavailableChunkAction::Skip:
                                continue;

                            case EUnavailableChunkAction::Wait:
                                // Do nothing.
                                break;

                            default:
                                Y_UNREACHABLE();
                        }
                    }
                    result.back().push_back(CreateUnversionedInputDataSlice(CreateInputChunkSlice(
                        inputChunk,
                        GetKeyPrefix(inputChunk->BoundaryKeys()->MinKey.Get(), foreignKeyColumnCount, RowBuffer),
                        GetKeyPrefixSuccessor(inputChunk->BoundaryKeys()->MaxKey.Get(), foreignKeyColumnCount, RowBuffer))));
                }
            }
        }
    }
    return result;
}

bool TOperationControllerBase::InputHasDynamicTables() const
{
    for (const auto& table : InputTables) {
        if (table.IsDynamic) {
            return true;
        }
    }
    return false;
}

bool TOperationControllerBase::InputHasVersionedTables() const
{
    for (const auto& table : InputTables) {
        if (table.IsDynamic && table.Schema.IsSorted()) {
            return true;
        }
    }
    return false;
}

bool TOperationControllerBase::InputHasReadLimits() const
{
    for (const auto& table : InputTables) {
        if (table.Path.HasNontrivialRanges()) {
            return true;
        }
    }
    return false;
}

bool TOperationControllerBase::IsLocalityEnabled() const
{
    return Config->EnableLocality && TotalEstimatedInputDataWeight > Spec_->MinLocalityInputDataWeight;
}

TString TOperationControllerBase::GetLoggingProgress() const
{
    return Format(
        "Jobs = {T: %v, R: %v, C: %v, P: %v, F: %v, A: %v, I: %v}, "
        "UnavailableInputChunks: %v",
        JobCounter->GetTotal(),
        JobCounter->GetRunning(),
        JobCounter->GetCompletedTotal(),
        GetPendingJobCount(),
        JobCounter->GetFailed(),
        JobCounter->GetAbortedTotal(),
        JobCounter->GetInterruptedTotal(),
        GetUnavailableInputChunkCount());
}

void TOperationControllerBase::SlicePrimaryVersionedChunks(
    const IJobSizeConstraintsPtr& jobSizeConstraints,
    std::vector<TChunkStripePtr>* result)
{
    for (const auto& dataSlice : CollectPrimaryVersionedDataSlices(jobSizeConstraints->GetInputSliceDataWeight())) {
        result->push_back(New<TChunkStripe>(dataSlice));
    }
}

bool TOperationControllerBase::IsJobInterruptible() const
{
    return Spec_->AutoMerge->Mode == EAutoMergeMode::Disabled;
}

void TOperationControllerBase::ReinstallUnreadInputDataSlices(
    const std::vector<NChunkClient::TInputDataSlicePtr>& inputDataSlices)
{
    Y_UNREACHABLE();
}

void TOperationControllerBase::ExtractInterruptDescriptor(TCompletedJobSummary& jobSummary) const
{
    std::vector<TInputDataSlicePtr> dataSliceList;

    const auto& result = jobSummary.Result;
    const auto& schedulerResultExt = result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    std::vector<TDataSliceDescriptor> unreadDataSliceDescriptors;
    std::vector<TDataSliceDescriptor> readDataSliceDescriptors;
    if (schedulerResultExt.unread_chunk_specs_size() > 0) {
        FromProto(
            &unreadDataSliceDescriptors,
            schedulerResultExt.unread_chunk_specs(),
            schedulerResultExt.chunk_spec_count_per_unread_data_slice());
    }
    if (schedulerResultExt.read_chunk_specs_size() > 0) {
        FromProto(
            &readDataSliceDescriptors,
            schedulerResultExt.read_chunk_specs(),
            schedulerResultExt.chunk_spec_count_per_read_data_slice());
    }

    auto extractDataSlice = [&] (const TDataSliceDescriptor& dataSliceDescriptor) {
        std::vector<TInputChunkSlicePtr> chunkSliceList;
        chunkSliceList.reserve(dataSliceDescriptor.ChunkSpecs.size());
        for (const auto& protoChunkSpec : dataSliceDescriptor.ChunkSpecs) {
            auto chunkId = FromProto<TChunkId>(protoChunkSpec.chunk_id());
            auto it = InputChunkMap.find(chunkId);
            YCHECK(it != InputChunkMap.end());
            const auto& inputChunks = it->second.InputChunks;
            auto chunkIt = std::find_if(
                inputChunks.begin(),
                inputChunks.end(),
                [&] (const TInputChunkPtr& inputChunk) -> bool {
                    return inputChunk->GetChunkIndex() == protoChunkSpec.chunk_index();
                });
            YCHECK(chunkIt != inputChunks.end());
            auto chunkSlice = New<TInputChunkSlice>(*chunkIt, RowBuffer, protoChunkSpec);
            chunkSliceList.emplace_back(std::move(chunkSlice));
        }
        TInputDataSlicePtr dataSlice;
        if (InputTables[dataSliceDescriptor.GetDataSourceIndex()].IsDynamic) {
            dataSlice = CreateVersionedInputDataSlice(chunkSliceList);
        } else {
            YCHECK(chunkSliceList.size() == 1);
            dataSlice = CreateUnversionedInputDataSlice(chunkSliceList[0]);
        }
        dataSlice->Tag = dataSliceDescriptor.GetTag();
        return dataSlice;
    };

    for (const auto& dataSliceDescriptor : unreadDataSliceDescriptors) {
        jobSummary.UnreadInputDataSlices.emplace_back(extractDataSlice(dataSliceDescriptor));
    }
    for (const auto& dataSliceDescriptor : readDataSliceDescriptors) {
        jobSummary.ReadInputDataSlices.emplace_back(extractDataSlice(dataSliceDescriptor));
    }
}

int TOperationControllerBase::EstimateSplitJobCount(const TCompletedJobSummary& jobSummary, const TJobletPtr& joblet)
{
    int jobCount = 1;

    if (JobSplitter_ && GetPendingJobCount() == 0) {
        auto inputDataStatistics = GetTotalInputDataStatistics(*jobSummary.Statistics);

        // We don't estimate unread row count based on unread slices,
        // because foreign slices are not passed back to scheduler.
        // Instead, we take the difference between estimated row count and actual read row count.
        i64 unreadRowCount = joblet->InputStripeList->TotalRowCount - inputDataStatistics.row_count();

        if (unreadRowCount <= 0) {
            // This is almost impossible, still we don't want to fail operation in this case.
            LOG_WARNING("Estimated unread row count is negative (JobId: %v, UnreadRowCount: %v)", jobSummary.Id, unreadRowCount);
            unreadRowCount = 1;
        }

        jobCount = JobSplitter_->EstimateJobCount(jobSummary, unreadRowCount);
    }
    return jobCount;
}

TKeyColumns TOperationControllerBase::CheckInputTablesSorted(
    const TKeyColumns& keyColumns,
    std::function<bool(const TInputTable& table)> inputTableFilter)
{
    YCHECK(!InputTables.empty());

    for (const auto& table : InputTables) {
        if (inputTableFilter(table) && !table.Schema.IsSorted()) {
            THROW_ERROR_EXCEPTION("Input table %v is not sorted",
                table.Path.GetPath());
        }
    }

    auto validateColumnFilter = [] (const TInputTable& table, const TKeyColumns& keyColumns) {
        auto columns = table.Path.GetColumns();
        if (!columns) {
            return;
        }

        auto columnSet = THashSet<TString>(columns->begin(), columns->end());
        for (const auto& keyColumn : keyColumns) {
            if (columnSet.find(keyColumn) == columnSet.end()) {
                THROW_ERROR_EXCEPTION("Column filter for input table %v doesn't include key column %Qv",
                    table.Path.GetPath(),
                    keyColumn);
            }
        }
    };

    if (!keyColumns.empty()) {
        for (const auto& table : InputTables) {
            if (!inputTableFilter(table)) {
                continue;
            }

            if (!CheckKeyColumnsCompatible(table.Schema.GetKeyColumns(), keyColumns)) {
                THROW_ERROR_EXCEPTION("Input table %v is sorted by columns %v that are not compatible "
                    "with the requested columns %v",
                    table.Path.GetPath(),
                    table.Schema.GetKeyColumns(),
                    keyColumns);
            }
            validateColumnFilter(table, keyColumns);
        }
        return keyColumns;
    } else {
        for (const auto& referenceTable : InputTables) {
            if (inputTableFilter(referenceTable)) {
                for (const auto& table : InputTables) {
                    if (!inputTableFilter(table)) {
                        continue;
                    }

                    if (table.Schema.GetKeyColumns() != referenceTable.Schema.GetKeyColumns()) {
                        THROW_ERROR_EXCEPTION("Key columns do not match: input table %v is sorted by columns %v "
                            "while input table %v is sorted by columns %v",
                            table.Path.GetPath(),
                            table.Schema.GetKeyColumns(),
                            referenceTable.Path.GetPath(),
                            referenceTable.Schema.GetKeyColumns());
                    }
                    validateColumnFilter(table, referenceTable.Schema.GetKeyColumns());
                }
                return referenceTable.Schema.GetKeyColumns();
            }
        }
    }
    Y_UNREACHABLE();
}

bool TOperationControllerBase::CheckKeyColumnsCompatible(
    const TKeyColumns& fullColumns,
    const TKeyColumns& prefixColumns)
{
    if (fullColumns.size() < prefixColumns.size()) {
        return false;
    }

    for (int index = 0; index < prefixColumns.size(); ++index) {
        if (fullColumns[index] != prefixColumns[index]) {
            return false;
        }
    }

    return true;
}

bool TOperationControllerBase::ShouldVerifySortedOutput() const
{
    return true;
}

TOutputOrderPtr TOperationControllerBase::GetOutputOrder() const
{
    return nullptr;
}

bool TOperationControllerBase::CheckParityReplicas() const
{
    return false;
}

bool TOperationControllerBase::IsBoundaryKeysFetchEnabled() const
{
    return false;
}

void TOperationControllerBase::AttachToIntermediateLivePreview(const TChunkId& chunkId)
{
    if (IsIntermediateLivePreviewSupported()) {
        AttachToLivePreview(chunkId, IntermediateTable.LivePreviewTableId);
    }
}

void TOperationControllerBase::AttachToLivePreview(
    TChunkTreeId chunkTreeId,
    NCypressClient::TNodeId tableId)
{
    Host->AttachChunkTreesToLivePreview(
        AsyncTransaction->GetId(),
        tableId,
        {chunkTreeId});
}

void TOperationControllerBase::RegisterStderr(const TJobletPtr& joblet, const TJobSummary& jobSummary)
{
    if (!joblet->StderrTableChunkListId) {
        return;
    }

    YCHECK(StderrTable_);

    const auto& chunkListId = joblet->StderrTableChunkListId;
    const auto& result = jobSummary.Result;

    if (!result.HasExtension(TSchedulerJobResultExt::scheduler_job_result_ext)) {
        return;
    }
    const auto& schedulerResultExt = result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    YCHECK(schedulerResultExt.has_stderr_table_boundary_keys());

    const auto& boundaryKeys = schedulerResultExt.stderr_table_boundary_keys();
    if (boundaryKeys.empty()) {
        return;
    }
    auto key = BuildBoundaryKeysFromOutputResult(boundaryKeys, StderrTable_->GetEdgeDescriptorTemplate(), RowBuffer);
    StderrTable_->OutputChunkTreeIds.emplace_back(key, chunkListId);

    LOG_DEBUG("Stderr chunk tree registered (ChunkListId: %v)",
        chunkListId);
}

void TOperationControllerBase::RegisterCores(const TJobletPtr& joblet, const TJobSummary& jobSummary)
{
    if (!joblet->CoreTableChunkListId) {
        return;
    }

    YCHECK(CoreTable_);

    const auto& chunkListId = joblet->CoreTableChunkListId;
    const auto& result = jobSummary.Result;

    if (!result.HasExtension(TSchedulerJobResultExt::scheduler_job_result_ext)) {
        return;
    }
    const auto& schedulerResultExt = result.GetExtension(TSchedulerJobResultExt::scheduler_job_result_ext);

    for (const auto& coreInfo : schedulerResultExt.core_infos()) {
        LOG_DEBUG("Core file (JobId: %v, ProcessId: %v, ExecutableName: %v, Size: %v, Error: %v)",
            joblet->JobId,
            coreInfo.process_id(),
            coreInfo.executable_name(),
            coreInfo.size(),
            coreInfo.has_error() ? FromProto<TError>(coreInfo.error()) : TError());
    }

    const auto& boundaryKeys = schedulerResultExt.core_table_boundary_keys();
    if (boundaryKeys.empty()) {
        return;
    }
    auto key = BuildBoundaryKeysFromOutputResult(boundaryKeys, CoreTable_->GetEdgeDescriptorTemplate(), RowBuffer);
    CoreTable_->OutputChunkTreeIds.emplace_back(key, chunkListId);
}

const ITransactionPtr& TOperationControllerBase::GetTransactionForOutputTable(const TOutputTable& table) const
{
    if (table.OutputType == EOutputTableType::Output) {
        if (OutputCompletionTransaction) {
            return OutputCompletionTransaction;
        } else {
            return OutputTransaction;
        }
    } else {
        YCHECK(table.OutputType == EOutputTableType::Stderr || table.OutputType == EOutputTableType::Core);
        if (DebugCompletionTransaction) {
            return DebugCompletionTransaction;
        } else {
            return DebugTransaction;
        }
    }
}

void TOperationControllerBase::RegisterTeleportChunk(
    TInputChunkPtr chunkSpec,
    TChunkStripeKey key,
    int tableIndex)
{
    auto& table = OutputTables_[tableIndex];

    if (table.TableUploadOptions.TableSchema.IsSorted() && ShouldVerifySortedOutput()) {
        YCHECK(chunkSpec->BoundaryKeys());
        YCHECK(chunkSpec->GetRowCount() > 0);
        YCHECK(chunkSpec->GetUniqueKeys() || !table.Options->ValidateUniqueKeys);

        TOutputResult resultBoundaryKeys;
        resultBoundaryKeys.set_empty(false);
        resultBoundaryKeys.set_sorted(true);
        resultBoundaryKeys.set_unique_keys(chunkSpec->GetUniqueKeys());
        ToProto(resultBoundaryKeys.mutable_min(), chunkSpec->BoundaryKeys()->MinKey);
        ToProto(resultBoundaryKeys.mutable_max(), chunkSpec->BoundaryKeys()->MaxKey);

        key = BuildBoundaryKeysFromOutputResult(resultBoundaryKeys, StandardEdgeDescriptors_[tableIndex], RowBuffer);
    }

    table.OutputChunkTreeIds.emplace_back(key, chunkSpec->ChunkId());

    if (IsOutputLivePreviewSupported()) {
        AttachToLivePreview(chunkSpec->ChunkId(), table.LivePreviewTableId);
    }

    LOG_DEBUG("Teleport chunk registered (Table: %v, ChunkId: %v, Key: %v)",
        tableIndex,
        chunkSpec->ChunkId(),
        key);
}

void TOperationControllerBase::RegisterInputStripe(const TChunkStripePtr& stripe, const TTaskPtr& task)
{
    THashSet<TChunkId> visitedChunks;

    TStripeDescriptor stripeDescriptor;
    stripeDescriptor.Stripe = stripe;
    stripeDescriptor.Task = task;
    stripeDescriptor.Cookie = task->GetChunkPoolInput()->Add(stripe);

    for (const auto& dataSlice : stripe->DataSlices) {
        for (const auto& slice : dataSlice->ChunkSlices) {
            auto inputChunk = slice->GetInputChunk();
            const auto& chunkId = inputChunk->ChunkId();

            if (!visitedChunks.insert(chunkId).second) {
                continue;
            }

            auto chunkDescriptorIt = InputChunkMap.find(chunkId);
            YCHECK(chunkDescriptorIt != InputChunkMap.end());

            auto& chunkDescriptor = chunkDescriptorIt->second;
            chunkDescriptor.InputStripes.push_back(stripeDescriptor);

            if (chunkDescriptor.State == EInputChunkState::Waiting) {
                ++stripe->WaitingChunkCount;
            }
        }
    }

    if (stripe->WaitingChunkCount > 0) {
        task->GetChunkPoolInput()->Suspend(stripeDescriptor.Cookie);
    }
}

void TOperationControllerBase::RegisterRecoveryInfo(
    const TCompletedJobPtr& completedJob,
    const TChunkStripePtr& stripe)
{
    for (const auto& dataSlice : stripe->DataSlices) {
        // NB: intermediate slice must be trivial.
        const auto& chunkId = dataSlice->GetSingleUnversionedChunkOrThrow()->ChunkId();
        YCHECK(ChunkOriginMap.emplace(chunkId, completedJob).second);
    }

    IntermediateChunkScraper->Restart();
}

TRowBufferPtr TOperationControllerBase::GetRowBuffer()
{
    return RowBuffer;
}

TSnapshotCookie TOperationControllerBase::OnSnapshotStarted()
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    if (RecentSnapshotIndex_) {
        LOG_WARNING("Starting next snapshot without completing previous one (SnapshotIndex: %v)",
            SnapshotIndex_);
    }
    RecentSnapshotIndex_ = SnapshotIndex_++;

    CompletedJobIdsSnapshotCookie_ = CompletedJobIdsReleaseQueue_.Checkpoint();
    IntermediateStripeListSnapshotCookie_ = IntermediateStripeListReleaseQueue_.Checkpoint();
    ChunkTreeSnapshotCookie_ = ChunkTreeReleaseQueue_.Checkpoint();
    LOG_INFO("Storing snapshot cookies (CompletedJobIdsSnapshotCookie: %v, StripeListSnapshotCookie: %v, "
        "ChunkTreeSnapshotCookie: %v, SnapshotIndex: %v)",
        CompletedJobIdsSnapshotCookie_,
        IntermediateStripeListSnapshotCookie_,
        ChunkTreeSnapshotCookie_,
        *RecentSnapshotIndex_);

    TSnapshotCookie result;
    result.SnapshotIndex = *RecentSnapshotIndex_;
    return result;
}

void TOperationControllerBase::SafeOnSnapshotCompleted(const TSnapshotCookie& cookie)
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    // OnSnapshotCompleted should match the most recent OnSnapshotStarted.
    YCHECK(RecentSnapshotIndex_);
    YCHECK(cookie.SnapshotIndex == *RecentSnapshotIndex_);

    // Completed job ids.
    {
        auto headCookie = CompletedJobIdsReleaseQueue_.GetHeadCookie();
        auto jobIdsToRelease = CompletedJobIdsReleaseQueue_.Release(CompletedJobIdsSnapshotCookie_);
        LOG_INFO("Releasing jobs on snapshot completion (SnapshotCookie: %v, HeadCookie: %v, JobCount: %v, SnapshotIndex: %v)",
            CompletedJobIdsSnapshotCookie_,
            headCookie,
            jobIdsToRelease.size(),
            cookie.SnapshotIndex);
        ReleaseJobs(jobIdsToRelease);
    }

    // Stripe lists.
    {
        auto headCookie = IntermediateStripeListReleaseQueue_.GetHeadCookie();
        auto stripeListsToRelease = IntermediateStripeListReleaseQueue_.Release(IntermediateStripeListSnapshotCookie_);
        LOG_INFO("Releasing stripe lists (SnapshotCookie: %v, HeadCookie: %v, StripeListCount: %v, SnapshotIndex: %v)",
            IntermediateStripeListSnapshotCookie_,
            headCookie,
            stripeListsToRelease.size(),
            cookie.SnapshotIndex);

        for (const auto& stripeList : stripeListsToRelease) {
            auto chunks = GetStripeListChunks(stripeList);
            AddChunksToUnstageList(std::move(chunks));
            OnChunksReleased(stripeList->TotalChunkCount);
        }
    }

    // Chunk trees.
    {
        auto headCookie = ChunkTreeReleaseQueue_.GetHeadCookie();
        auto chunkTreeIdsToRelease = ChunkTreeReleaseQueue_.Release(ChunkTreeSnapshotCookie_);
        LOG_INFO("Releasing chunk trees (SnapshotCookie: %v, HeadCookie: %v, ChunkTreeCount: %v, SnapshotIndex: %v)",
            ChunkTreeSnapshotCookie_,
            headCookie,
            chunkTreeIdsToRelease.size(),
            cookie.SnapshotIndex);

        Host->AddChunkTreesToUnstageList(chunkTreeIdsToRelease, true /* recursive */);
    }

    RecentSnapshotIndex_.Reset();
    LastSuccessfulSnapshotTime_ = TInstant::Now();
}

void TOperationControllerBase::Dispose()
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    auto headCookie = CompletedJobIdsReleaseQueue_.Checkpoint();
    LOG_INFO("Releasing jobs on controller disposal (HeadCookie: %v)",
        headCookie);
    auto jobIdsToRelease = CompletedJobIdsReleaseQueue_.Release();
    ReleaseJobs(jobIdsToRelease);
}

NScheduler::TOperationJobMetrics TOperationControllerBase::PullJobMetricsDelta()
{
    TGuard<TSpinLock> guard(JobMetricsDeltaPerTreeLock_);

    auto now = NProfiling::GetCpuInstant();
    if (LastJobMetricsDeltaReportTime_ + DurationToCpuDuration(Config->JobMetricsReportPeriod) > now) {
        return {};
    }

    NScheduler::TOperationJobMetrics result;
    for (auto& pair : JobMetricsDeltaPerTree_) {
        const auto& treeId = pair.first;
        auto& delta = pair.second;
        if (!delta.IsEmpty()) {
            result.push_back({treeId, delta});
            delta = NScheduler::TJobMetrics();
        }
    }
    LastJobMetricsDeltaReportTime_ = now;

    LOG_DEBUG_UNLESS(result.empty(), "Non-zero job metrics reported");

    return result;
}

TOperationAlertMap TOperationControllerBase::GetAlerts()
{
    TGuard<TSpinLock> guard(AlertsLock_);
    return Alerts_;
}

TOperationInfo TOperationControllerBase::BuildOperationInfo()
{
    TOperationInfo result;

    result.Progress =
        BuildYsonStringFluently<EYsonType::MapFragment>()
            .Do(std::bind(&TOperationControllerBase::BuildProgress, this, _1))
        .Finish();

    result.BriefProgress =
        BuildYsonStringFluently<EYsonType::MapFragment>()
            .Do(std::bind(&TOperationControllerBase::BuildBriefProgress, this, _1))
        .Finish();

    result.RunningJobs =
        BuildYsonStringFluently<EYsonType::MapFragment>()
            .Do(std::bind(&TOperationControllerBase::BuildJobsYson, this, _1))
        .Finish();

    result.JobSplitter =
        BuildYsonStringFluently<EYsonType::MapFragment>()
            .Do(std::bind(&TOperationControllerBase::BuildJobSplitterInfo, this, _1))
        .Finish();

    result.MemoryUsage = GetMemoryUsage();

    result.ControllerState = State;

    return result;
}

ssize_t TOperationControllerBase::GetMemoryUsage() const
{
    return GetMemoryUsageForTag(MemoryTag_);
}

bool TOperationControllerBase::HasEnoughChunkLists(bool isWritingStderrTable, bool isWritingCoreTable)
{
    // We use this "result" variable to make sure that we have enough chunk lists
    // for every cell tag and start allocating them all in advance and simultaneously.
    bool result = true;
    for (const auto& pair : CellTagToRequiredOutputChunkLists_) {
        const auto cellTag = pair.first;
        auto requiredChunkList = pair.second;
        if (requiredChunkList && !OutputChunkListPool_->HasEnough(cellTag, requiredChunkList)) {
            result = false;
        }
    }
    for (const auto& pair : CellTagToRequiredDebugChunkLists_) {
        const auto cellTag = pair.first;
        auto requiredChunkList = pair.second;
        if (StderrTable_ && !isWritingStderrTable && StderrTable_->CellTag == cellTag) {
            --requiredChunkList;
        }
        if (CoreTable_ && !isWritingCoreTable && CoreTable_->CellTag == cellTag) {
            --requiredChunkList;
        }
        if (requiredChunkList && !DebugChunkListPool_->HasEnough(cellTag, requiredChunkList)) {
            result = false;
        }
    }
    return result;
}

TChunkListId TOperationControllerBase::ExtractOutputChunkList(TCellTag cellTag)
{
    return OutputChunkListPool_->Extract(cellTag);
}

TChunkListId TOperationControllerBase::ExtractDebugChunkList(TCellTag cellTag)
{
    return DebugChunkListPool_->Extract(cellTag);
}

void TOperationControllerBase::ReleaseChunkTrees(
    const std::vector<TChunkListId>& chunkTreeIds,
    bool unstageRecursively,
    bool waitForSnapshot)
{
    if (waitForSnapshot) {
        YCHECK(unstageRecursively);
        for (const auto& chunkTreeId : chunkTreeIds) {
            ChunkTreeReleaseQueue_.Push(chunkTreeId);
        }
    } else {
        Host->AddChunkTreesToUnstageList(chunkTreeIds, unstageRecursively);
    }
}

void TOperationControllerBase::RegisterJoblet(const TJobletPtr& joblet)
{
    YCHECK(JobletMap.insert(std::make_pair(joblet->JobId, joblet)).second);
}

TJobletPtr TOperationControllerBase::FindJoblet(const TJobId& jobId) const
{
    auto it = JobletMap.find(jobId);
    return it == JobletMap.end() ? nullptr : it->second;
}

TJobletPtr TOperationControllerBase::GetJoblet(const TJobId& jobId) const
{
    auto joblet = FindJoblet(jobId);
    YCHECK(joblet);
    return joblet;
}

TJobletPtr TOperationControllerBase::GetJobletOrThrow(const TJobId& jobId) const
{
    auto joblet = FindJoblet(jobId);
    if (!joblet) {
        THROW_ERROR_EXCEPTION(
            NScheduler::EErrorCode::NoSuchJob,
            "No such job %v",
            jobId);
    }
    return joblet;
}

void TOperationControllerBase::UnregisterJoblet(const TJobletPtr& joblet)
{
    const auto& jobId = joblet->JobId;
    YCHECK(JobletMap.erase(jobId) == 1);
}

void TOperationControllerBase::SetProgressUpdated()
{
    ShouldUpdateProgressInCypress_.store(false);
}

bool TOperationControllerBase::ShouldUpdateProgress() const
{
    return HasProgress() && ShouldUpdateProgressInCypress_;
}

bool TOperationControllerBase::HasProgress() const
{
    if (!IsPrepared()) {
        return false;
    }

    {
        TGuard<TSpinLock> guard(ProgressLock_);
        return ProgressString_ && BriefProgressString_;
    }
}

void TOperationControllerBase::BuildInitializeMutableAttributes(TFluentMap fluent) const
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    fluent
        .Item("async_scheduler_transaction_id").Value(AsyncTransaction ? AsyncTransaction->GetId() : NullTransactionId)
        .Item("input_transaction_id").Value(InputTransaction ? InputTransaction->GetId() : NullTransactionId)
        .Item("output_transaction_id").Value(OutputTransaction ? OutputTransaction->GetId() : NullTransactionId)
        .Item("debug_transaction_id").Value(DebugTransaction ? DebugTransaction->GetId() : NullTransactionId);
}

void TOperationControllerBase::BuildPrepareAttributes(TFluentMap fluent) const
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    fluent
        .DoIf(static_cast<bool>(AutoMergeDirector_), [&] (TFluentMap fluent) {
            fluent
                .Item("auto_merge").BeginMap()
                    .Item("max_intermediate_chunk_count").Value(AutoMergeDirector_->GetMaxIntermediateChunkCount())
                    .Item("chunk_count_per_merge_job").Value(AutoMergeDirector_->GetChunkCountPerMergeJob())
                .EndMap();
        });
}

void TOperationControllerBase::BuildBriefSpec(TFluentMap fluent) const
{
    std::vector<TYPath> inputPaths;
    for (const auto& path : GetInputTablePaths()) {
        inputPaths.push_back(path.GetPath());
    }

    std::vector<TYPath> outputPaths;
    for (const auto& path : GetOutputTablePaths()) {
        outputPaths.push_back(path.GetPath());
    }

    fluent
        .DoIf(Spec_->Title.HasValue(), [&] (TFluentMap fluent) {
            fluent
                .Item("title").Value(*Spec_->Title);
        })
        .Item("input_table_paths").ListLimited(inputPaths, 1)
        .Item("output_table_paths").ListLimited(outputPaths, 1);
}

void TOperationControllerBase::BuildProgress(TFluentMap fluent) const
{
    if (!IsPrepared()) {
        return;
    }

    fluent
        .Item("build_time").Value(TInstant::Now())
        .Item("jobs").Value(JobCounter)
        .Item("ready_job_count").Value(GetPendingJobCount())
        .Item("job_statistics").Value(JobStatistics)
        .Item("estimated_input_statistics").BeginMap()
            .Item("chunk_count").Value(TotalEstimatedInputChunkCount)
            .Item("uncompressed_data_size").Value(TotalEstimatedInputUncompressedDataSize)
            .Item("compressed_data_size").Value(TotalEstimatedInputCompressedDataSize)
            .Item("data_weight").Value(TotalEstimatedInputDataWeight)
            .Item("row_count").Value(TotalEstimatedInputRowCount)
            .Item("unavailable_chunk_count").Value(GetUnavailableInputChunkCount() + UnavailableIntermediateChunkCount)
            .Item("data_slice_count").Value(GetDataSliceCount())
        .EndMap()
        .Item("live_preview").BeginMap()
            .Item("output_supported").Value(IsOutputLivePreviewSupported())
            .Item("intermediate_supported").Value(IsIntermediateLivePreviewSupported())
            .Item("stderr_supported").Value(StderrTable_.HasValue())
        .EndMap()
        .Item("schedule_job_statistics").BeginMap()
            .Item("count").Value(ScheduleJobStatistics_->Count)
            .Item("duration").Value(ScheduleJobStatistics_->Duration)
            .Item("failed").Value(ScheduleJobStatistics_->Failed)
        .EndMap()
		.DoIf(DataFlowGraph_.operator bool(), [=] (TFluentMap fluent) {
		    fluent
                .Item("data_flow_graph").BeginMap()
                    .Do(BIND(&TDataFlowGraph::BuildLegacyYson, DataFlowGraph_))
                .EndMap();
		})
        .DoIf(EstimatedInputDataSizeHistogram_.operator bool(), [=] (TFluentMap fluent) {
            EstimatedInputDataSizeHistogram_->BuildHistogramView();
            fluent
                .Item("estimated_input_data_size_histogram").Value(*EstimatedInputDataSizeHistogram_);
        })
        .DoIf(InputDataSizeHistogram_.operator bool(), [=] (TFluentMap fluent) {
            InputDataSizeHistogram_->BuildHistogramView();
            fluent
                .Item("input_data_size_histogram").Value(*InputDataSizeHistogram_);
        })
        .Item("snapshot_index").Value(SnapshotIndex_)
        .Item("recent_snapshot_index").Value(RecentSnapshotIndex_)
        .Item("last_successful_snapshot_time").Value(LastSuccessfulSnapshotTime_);
}

void TOperationControllerBase::BuildBriefProgress(TFluentMap fluent) const
{
    if (!IsPrepared()) {
        return;
    }

    fluent
        .Item("jobs").Do(BIND(&SerializeBriefVersion, JobCounter));
}

void TOperationControllerBase::BuildAndSaveProgress()
{
    auto progressString = BuildYsonStringFluently()
        .BeginMap()
        .Do([=] (TFluentMap fluent) {
            auto asyncResult = WaitFor(
                BIND(&TOperationControllerBase::BuildProgress, MakeStrong(this))
                    .AsyncVia(GetInvoker())
                    .Run(fluent));
                asyncResult
                    .ThrowOnError();
            })
        .EndMap();

    auto briefProgressString = BuildYsonStringFluently()
        .BeginMap()
            .Do([=] (TFluentMap fluent) {
                auto asyncResult = WaitFor(
                    BIND(&TOperationControllerBase::BuildBriefProgress, MakeStrong(this))
                        .AsyncVia(GetInvoker())
                        .Run(fluent));
                asyncResult
                    .ThrowOnError();
            })
        .EndMap();

    {
        TGuard<TSpinLock> guard(ProgressLock_);
        if (!ProgressString_ || ProgressString_ != progressString ||
            !BriefProgressString_ || BriefProgressString_ != briefProgressString)
        {
            ShouldUpdateProgressInCypress_.store(true);
        }
        ProgressString_ = progressString;
        BriefProgressString_ = briefProgressString;
    }
}

TYsonString TOperationControllerBase::GetProgress() const
{
    TGuard<TSpinLock> guard(ProgressLock_);
    return ProgressString_;
}

TYsonString TOperationControllerBase::GetBriefProgress() const
{
    TGuard<TSpinLock> guard(ProgressLock_);
    return BriefProgressString_;
}

TYsonString TOperationControllerBase::BuildJobYson(const TJobId& id, bool outputStatistics) const
{
    TCallback<void(TFluentMap)> attributesBuilder;

    // Case of running job.
    {
        auto joblet = FindJoblet(id);
        if (joblet) {
            attributesBuilder = BIND(
                &TOperationControllerBase::BuildJobAttributes,
                MakeStrong(this),
                joblet,
                EJobState::Running,
                outputStatistics);
        } else {
            attributesBuilder = BIND([] (TFluentMap) {});
        }
    }

    YCHECK(attributesBuilder);

    return BuildYsonStringFluently()
        .BeginMap()
            .Do(attributesBuilder)
        .EndMap();
}

IYPathServicePtr TOperationControllerBase::GetOrchid() const
{
    if (CancelableContext->IsCanceled()) {
        return nullptr;
    }
    return Orchid_;
}

void TOperationControllerBase::BuildJobsYson(TFluentMap fluent) const
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    auto now = GetInstant();
    if (CachedRunningJobsUpdateTime_ + Config->CachedRunningJobsUpdatePeriod < now) {
        CachedRunningJobsYson_ = BuildYsonStringFluently<EYsonType::MapFragment>()
            .DoFor(JobletMap, [&] (TFluentMap fluent, const std::pair<TJobId, TJobletPtr>& pair) {
                const auto& jobId = pair.first;
                const auto& joblet = pair.second;
                if (joblet->StartTime) {
                    fluent.Item(ToString(jobId)).BeginMap()
                        .Do([&] (TFluentMap fluent) {
                            BuildJobAttributes(joblet, EJobState::Running, /* outputStatistics */ false, fluent);
                        })
                    .EndMap();
                }
            })
            .Finish();
        CachedRunningJobsUpdateTime_ = now;
    }

    fluent.GetConsumer()->OnRaw(CachedRunningJobsYson_);
}

TSharedRef TOperationControllerBase::ExtractJobSpec(const TJobId& jobId) const
{
    if (Spec_->TestingOperationOptions->FailGetJobSpec) {
        THROW_ERROR_EXCEPTION("Testing failure");
    }

    auto joblet = GetJobletOrThrow(jobId);
    if (!joblet->JobSpecProtoFuture) {
        THROW_ERROR_EXCEPTION("Spec of job %v is missing", jobId);
    }

    auto result = WaitFor(joblet->JobSpecProtoFuture)
        .ValueOrThrow();
    joblet->JobSpecProtoFuture.Reset();

    return result;
}

TYsonString TOperationControllerBase::GetSuspiciousJobsYson() const
{
    VERIFY_THREAD_AFFINITY_ANY();

    TReaderGuard guard(CachedSuspiciousJobsYsonLock_);
    return CachedSuspiciousJobsYson_;
}

void TOperationControllerBase::UpdateSuspiciousJobsYson()
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    // We sort suspicious jobs by their last activity time and then
    // leave top `MaxOrchidEntryCountPerType` for each job type.

    std::vector<TJobletPtr> suspiciousJoblets;
    for (const auto& pair : JobletMap) {
        const auto& joblet = pair.second;
        if (joblet->Suspicious) {
            suspiciousJoblets.emplace_back(joblet);
        }
    }

    std::sort(suspiciousJoblets.begin(), suspiciousJoblets.end(), [] (const TJobletPtr& lhs, const TJobletPtr& rhs) {
        return lhs->LastActivityTime < rhs->LastActivityTime;
    });

    THashMap<EJobType, int> suspiciousJobCountPerType;

    auto yson = BuildYsonStringFluently<EYsonType::MapFragment>()
        .DoFor(
            suspiciousJoblets,
            [&] (TFluentMap fluent, const TJobletPtr& joblet) {
                auto& count = suspiciousJobCountPerType[joblet->JobType];
                if (count < Config->SuspiciousJobs->MaxOrchidEntryCountPerType) {
                    ++count;
                    fluent.Item(ToString(joblet->JobId))
                        .BeginMap()
                            .Item("operation_id").Value(ToString(OperationId))
                            .Item("type").Value(FormatEnum(joblet->JobType))
                            .Item("brief_statistics").Value(joblet->BriefStatistics)
                            .Item("node").Value(joblet->NodeDescriptor.Address)
                            .Item("last_activity_time").Value(joblet->LastActivityTime)
                        .EndMap();
                }
            })
        .Finish();

    {
        TWriterGuard guard(CachedSuspiciousJobsYsonLock_);
        CachedSuspiciousJobsYson_ = yson;
    }
}

void TOperationControllerBase::ReleaseJobs(const std::vector<TJobId>& jobIds)
{
    std::vector<TJobToRelease> jobsToRelease;
    jobsToRelease.reserve(jobIds.size());

    for (const auto& jobId : jobIds) {
        bool archiveJobSpec = false;
        bool archiveStderr = false;
        bool archiveFailContext = false;

        auto it = FinishedJobs_.find(jobId);
        if (it != FinishedJobs_.end()) {
            const auto& jobSummary = it->second->Summary;
            archiveJobSpec = jobSummary.ArchiveJobSpec;
            archiveStderr = jobSummary.ArchiveStderr;
            archiveFailContext = jobSummary.ArchiveFailContext;
            FinishedJobs_.erase(it);
        }
        jobsToRelease.emplace_back(TJobToRelease{jobId, archiveJobSpec, archiveStderr, archiveFailContext});
    }
    Host->ReleaseJobs(jobsToRelease);
}

void TOperationControllerBase::AnalyzeBriefStatistics(
    const TJobletPtr& job,
    const TSuspiciousJobsOptionsPtr& options,
    const TErrorOr<TBriefJobStatisticsPtr>& briefStatisticsOrError)
{
    if (!briefStatisticsOrError.IsOK()) {
        if (job->BriefStatistics) {
            // Failures in brief statistics building are normal during job startup,
            // when readers and writers are not built yet. After we successfully built
            // brief statistics once, we shouldn't fail anymore.

            LOG_WARNING(
                briefStatisticsOrError,
                "Failed to build brief job statistics (JobId: %v)",
                job->JobId);
        }

        return;
    }

    const auto& briefStatistics = briefStatisticsOrError.Value();

    bool wasActive = !job->BriefStatistics ||
        CheckJobActivity(
            job->BriefStatistics,
            briefStatistics,
            options);

    bool wasSuspicious = job->Suspicious;
    job->Suspicious = (!wasActive && briefStatistics->Timestamp - job->LastActivityTime > options->InactivityTimeout);
    if (!wasSuspicious && job->Suspicious) {
        LOG_DEBUG("Found a suspicious job (JobId: %v, LastActivityTime: %v, SuspiciousInactivityTimeout: %v, "
            "OldBriefStatistics: %v, NewBriefStatistics: %v)",
            job->JobId,
            job->LastActivityTime,
            options->InactivityTimeout,
            job->BriefStatistics,
            briefStatistics);
    }

    job->BriefStatistics = briefStatistics;

    if (wasActive) {
        job->LastActivityTime = job->BriefStatistics->Timestamp;
    }
}

void TOperationControllerBase::UpdateJobStatistics(const TJobletPtr& joblet, const TJobSummary& jobSummary)
{
    YCHECK(jobSummary.Statistics);

    // NB: There is a copy happening here that can be eliminated.
    auto statistics = *jobSummary.Statistics;
    LOG_TRACE("Job data statistics (JobId: %v, Input: %v, Output: %v)",
        jobSummary.Id,
        GetTotalInputDataStatistics(statistics),
        GetTotalOutputDataStatistics(statistics));

    auto statisticsState = GetStatisticsJobState(joblet, jobSummary.State);
    auto statisticsSuffix = JobHelper.GetStatisticsSuffix(statisticsState, joblet->JobType);
    statistics.AddSuffixToNames(statisticsSuffix);
    JobStatistics.Update(statistics);
}

void TOperationControllerBase::UpdateJobMetrics(const TJobletPtr& joblet, const TJobSummary& jobSummary)
{
    LOG_TRACE("Updating job metrics (JobId: %v)", joblet->JobId);

    auto delta = joblet->UpdateJobMetrics(jobSummary);
    {
        TGuard<TSpinLock> guard(JobMetricsDeltaPerTreeLock_);

        auto it = JobMetricsDeltaPerTree_.find(joblet->TreeId);
        if (it == JobMetricsDeltaPerTree_.end()) {
            YCHECK(JobMetricsDeltaPerTree_.insert(std::make_pair(joblet->TreeId, delta)).second);
        } else {
            it->second += delta;
        }
    }
}

void TOperationControllerBase::LogProgress(bool force)
{
    if (!HasProgress()) {
        return;
    }

    auto now = GetCpuInstant();
    if (force || now > NextLogProgressDeadline) {
        NextLogProgressDeadline = now + LogProgressBackoff;
        LOG_DEBUG("Progress: %v", GetLoggingProgress());
    }
}

TYsonString TOperationControllerBase::BuildInputPathYson(const TJobletPtr& joblet) const
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    if (!joblet->Task->SupportsInputPathYson()) {
        return TYsonString();
    }

    return BuildInputPaths(
        GetInputTablePaths(),
        joblet->InputStripeList,
        OperationType,
        joblet->JobType);
}

void TOperationControllerBase::BuildJobSplitterInfo(TFluentMap fluent) const
{
    VERIFY_INVOKER_AFFINITY(Invoker);

    if (IsPrepared() && JobSplitter_) {
        JobSplitter_->BuildJobSplitterInfo(fluent);
    }
}

ui64 TOperationControllerBase::NextJobIndex()
{
    return JobIndexGenerator.Next();
}

TOperationId TOperationControllerBase::GetOperationId() const
{
    return OperationId;
}

EOperationType TOperationControllerBase::GetOperationType() const
{
    return OperationType;
}

TCellTag TOperationControllerBase::GetIntermediateOutputCellTag() const
{
    return IntermediateOutputCellTag;
}

const TChunkListPoolPtr& TOperationControllerBase::GetOutputChunkListPool() const
{
    return OutputChunkListPool_;
}

const TControllerAgentConfigPtr& TOperationControllerBase::GetConfig() const
{
    return Config;
}

const TOperationSpecBasePtr& TOperationControllerBase::GetSpec() const
{
    return Spec_;
}

const TNullable<TOutputTable>& TOperationControllerBase::StderrTable() const
{
    return StderrTable_;
}

const TNullable<TOutputTable>& TOperationControllerBase::CoreTable() const
{
    return CoreTable_;
}

IJobSplitter* TOperationControllerBase::GetJobSplitter()
{
    return JobSplitter_.get();
}

const TNullable<TJobResources>& TOperationControllerBase::CachedMaxAvailableExecNodeResources() const
{
    return CachedMaxAvailableExecNodeResources_;
}

const TNodeDirectoryPtr& TOperationControllerBase::InputNodeDirectory() const
{
    return InputNodeDirectory_;
}

bool TOperationControllerBase::IsRowCountPreserved() const
{
    return false;
}

i64 TOperationControllerBase::GetUnavailableInputChunkCount() const
{
    if (!DataSliceFetcherChunkScrapers.empty() && State == EControllerState::Preparing) {
        i64 result = 0;
        for (const auto& fetcher : DataSliceFetcherChunkScrapers) {
            result += fetcher->GetUnavailableChunkCount();
        }
        return result;
    }
    return UnavailableInputChunkCount;
}

int TOperationControllerBase::GetTotalJobCount() const
{
    VERIFY_INVOKER_AFFINITY(CancelableInvoker);

    // Avoid accessing the state while not prepared.
    if (!IsPrepared()) {
        return 0;
    }

    return JobCounter->GetTotal();
}

i64 TOperationControllerBase::GetDataSliceCount() const
{
    i64 result = 0;
    for (const auto& task : Tasks) {
        result += task->GetInputDataSliceCount();
    }

    return result;
}

void TOperationControllerBase::InitUserJobSpecTemplate(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TUserJobSpecPtr config,
    const std::vector<TUserFile>& files,
    const TString& fileAccount)
{
    jobSpec->set_shell_command(config->Command);
    if (config->JobTimeLimit) {
        jobSpec->set_job_time_limit(config->JobTimeLimit.Get().MilliSeconds());
    }
    jobSpec->set_memory_limit(config->MemoryLimit);
    jobSpec->set_include_memory_mapped_files(config->IncludeMemoryMappedFiles);
    jobSpec->set_use_yamr_descriptors(config->UseYamrDescriptors);
    jobSpec->set_check_input_fully_consumed(config->CheckInputFullyConsumed);
    jobSpec->set_max_stderr_size(config->MaxStderrSize);
    jobSpec->set_custom_statistics_count_limit(config->CustomStatisticsCountLimit);
    jobSpec->set_copy_files(config->CopyFiles);
    jobSpec->set_file_account(fileAccount);

    jobSpec->set_port_count(config->PortCount);

    if (config->TmpfsPath && Config->EnableTmpfs) {
        auto tmpfsSize = config->TmpfsSize.Get(config->MemoryLimit);
        jobSpec->set_tmpfs_size(tmpfsSize);
        jobSpec->set_tmpfs_path(*config->TmpfsPath);
    }

    if (config->DiskSpaceLimit) {
        jobSpec->set_disk_space_limit(*config->DiskSpaceLimit);
    }
    if (config->InodeLimit) {
        jobSpec->set_inode_limit(*config->InodeLimit);
    }

    if (Config->IopsThreshold) {
        jobSpec->set_iops_threshold(*Config->IopsThreshold);
        if (Config->IopsThrottlerLimit) {
            jobSpec->set_iops_throttler_limit(*Config->IopsThrottlerLimit);
        }
    }

    {
        // Set input and output format.
        TFormat inputFormat(EFormatType::Yson);
        TFormat outputFormat(EFormatType::Yson);

        if (config->Format) {
            inputFormat = outputFormat = *config->Format;
        }

        if (config->InputFormat) {
            inputFormat = *config->InputFormat;
        }

        if (config->OutputFormat) {
            outputFormat = *config->OutputFormat;
        }

        jobSpec->set_input_format(ConvertToYsonString(inputFormat).GetData());
        jobSpec->set_output_format(ConvertToYsonString(outputFormat).GetData());
    }

    auto fillEnvironment = [&] (THashMap<TString, TString>& env) {
        for (const auto& pair : env) {
            jobSpec->add_environment(Format("%v=%v", pair.first, pair.second));
        }
    };

    // Global environment.
    fillEnvironment(Config->Environment);

    // Local environment.
    fillEnvironment(config->Environment);

    jobSpec->add_environment(Format("YT_OPERATION_ID=%v", OperationId));

    BuildFileSpecs(jobSpec, files);
}

const std::vector<TUserFile>& TOperationControllerBase::GetUserFiles(const TUserJobSpecPtr& userJobSpec) const
{
    auto it = UserJobFiles_.find(userJobSpec);
    YCHECK(it != UserJobFiles_.end());
    return it->second;
}

void TOperationControllerBase::InitUserJobSpec(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TJobletPtr joblet) const
{
    ToProto(jobSpec->mutable_debug_output_transaction_id(), DebugTransaction->GetId());

    i64 memoryReserve = joblet->EstimatedResourceUsage.GetUserJobMemory() * *joblet->UserJobMemoryReserveFactor;
    // Memory reserve should greater than or equal to tmpfs_size (see YT-5518 for more details).
    // This is ensured by adjusting memory reserve factor in user job config as initialization,
    // but just in case we also limit the actual memory_reserve value here.
    if (jobSpec->has_tmpfs_size()) {
        memoryReserve = std::max(memoryReserve, jobSpec->tmpfs_size());
    }
    jobSpec->set_memory_reserve(memoryReserve);

    jobSpec->add_environment(Format("YT_JOB_INDEX=%v", joblet->JobIndex));
    jobSpec->add_environment(Format("YT_TASK_JOB_INDEX=%v", joblet->TaskJobIndex));
    jobSpec->add_environment(Format("YT_JOB_ID=%v", joblet->JobId));
    if (joblet->StartRowIndex >= 0) {
        jobSpec->add_environment(Format("YT_START_ROW_INDEX=%v", joblet->StartRowIndex));
    }

    if (SecureVault) {
        // NB: These environment variables should be added to user job spec, not to the user job spec template.
        // They may contain sensitive information that should not be persisted with a controller.

        // We add a single variable storing the whole secure vault and all top-level scalar values.
        jobSpec->add_environment(Format("YT_SECURE_VAULT=%v",
            ConvertToYsonString(SecureVault, EYsonFormat::Text)));

        for (const auto& pair : SecureVault->GetChildren()) {
            TString value;
            auto node = pair.second;
            if (node->GetType() == ENodeType::Int64) {
                value = ToString(node->GetValue<i64>());
            } else if (node->GetType() == ENodeType::Uint64) {
                value = ToString(node->GetValue<ui64>());
            } else if (node->GetType() == ENodeType::Boolean) {
                value = ToString(node->GetValue<bool>());
            } else if (node->GetType() == ENodeType::Double) {
                value = ToString(node->GetValue<double>());
            } else if (node->GetType() == ENodeType::String) {
                value = node->GetValue<TString>();
            } else {
                // We do not export composite values as a separate environment variables.
                continue;
            }
            jobSpec->add_environment(Format("YT_SECURE_VAULT_%v=%v", pair.first, value));
        }
    }

    if (StderrCount_ >= Spec_->MaxStderrCount) {
        jobSpec->set_upload_stderr_if_completed(false);
    }

    if (joblet->StderrTableChunkListId) {
        AddStderrOutputSpecs(jobSpec, joblet);
    }
    if (joblet->CoreTableChunkListId) {
        AddCoreOutputSpecs(jobSpec, joblet);
    }
}

void TOperationControllerBase::AddStderrOutputSpecs(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TJobletPtr joblet) const
{
    auto* stderrTableSpec = jobSpec->mutable_stderr_table_spec();
    auto* outputSpec = stderrTableSpec->mutable_output_table_spec();
    outputSpec->set_table_writer_options(ConvertToYsonString(StderrTable_->Options).GetData());
    ToProto(outputSpec->mutable_table_schema(), StderrTable_->TableUploadOptions.TableSchema);
    ToProto(outputSpec->mutable_chunk_list_id(), joblet->StderrTableChunkListId);

    auto writerConfig = GetStderrTableWriterConfig();
    YCHECK(writerConfig);
    stderrTableSpec->set_blob_table_writer_config(ConvertToYsonString(writerConfig).GetData());
}

void TOperationControllerBase::AddCoreOutputSpecs(
    NScheduler::NProto::TUserJobSpec* jobSpec,
    TJobletPtr joblet) const
{
    auto* coreTableSpec = jobSpec->mutable_core_table_spec();
    auto* outputSpec = coreTableSpec->mutable_output_table_spec();
    outputSpec->set_table_writer_options(ConvertToYsonString(CoreTable_->Options).GetData());
    ToProto(outputSpec->mutable_table_schema(), CoreTable_->TableUploadOptions.TableSchema);
    ToProto(outputSpec->mutable_chunk_list_id(), joblet->CoreTableChunkListId);

    auto writerConfig = GetCoreTableWriterConfig();
    YCHECK(writerConfig);
    coreTableSpec->set_blob_table_writer_config(ConvertToYsonString(writerConfig).GetData());
}

void TOperationControllerBase::SetInputDataSources(TSchedulerJobSpecExt* jobSpec) const
{
    auto dataSourceDirectory = New<TDataSourceDirectory>();
    for (const auto& inputTable : InputTables) {
        auto dataSource = (inputTable.IsDynamic && inputTable.Schema.IsSorted())
            ? MakeVersionedDataSource(
                inputTable.GetPath(),
                inputTable.Schema,
                inputTable.Path.GetColumns(),
                inputTable.Path.GetTimestamp().Get(AsyncLastCommittedTimestamp))
            : MakeUnversionedDataSource(
                inputTable.GetPath(),
                inputTable.Schema,
                inputTable.Path.GetColumns());

        dataSourceDirectory->DataSources().push_back(dataSource);
    }
    NChunkClient::NProto::TDataSourceDirectoryExt dataSourceDirectoryExt;
    ToProto(&dataSourceDirectoryExt, dataSourceDirectory);
    SetProtoExtension(jobSpec->mutable_extensions(), dataSourceDirectoryExt);
}

void TOperationControllerBase::SetIntermediateDataSource(TSchedulerJobSpecExt* jobSpec) const
{
    auto dataSourceDirectory = New<TDataSourceDirectory>();
    dataSourceDirectory->DataSources().push_back(MakeUnversionedDataSource(
        IntermediatePath,
        Null,
        Null));
    NChunkClient::NProto::TDataSourceDirectoryExt dataSourceDirectoryExt;
    ToProto(&dataSourceDirectoryExt, dataSourceDirectory);
    SetProtoExtension(jobSpec->mutable_extensions(), dataSourceDirectoryExt);
}

i64 TOperationControllerBase::GetFinalOutputIOMemorySize(TJobIOConfigPtr ioConfig) const
{
    i64 result = 0;
    for (const auto& outputTable : OutputTables_) {
        if (outputTable.Options->ErasureCodec == NErasure::ECodec::None) {
            i64 maxBufferSize = std::max(
                ioConfig->TableWriter->MaxRowWeight,
                ioConfig->TableWriter->MaxBufferSize);
            result += GetOutputWindowMemorySize(ioConfig) + maxBufferSize;
        } else {
            auto* codec = NErasure::GetCodec(outputTable.Options->ErasureCodec);
            double replicationFactor = (double) codec->GetTotalPartCount() / codec->GetDataPartCount();
            result += static_cast<i64>(ioConfig->TableWriter->DesiredChunkSize * replicationFactor);
        }
    }
    return result;
}

i64 TOperationControllerBase::GetFinalIOMemorySize(
    TJobIOConfigPtr ioConfig,
    const TChunkStripeStatisticsVector& stripeStatistics) const
{
    i64 result = 0;
    for (const auto& stat : stripeStatistics) {
        result += GetInputIOMemorySize(ioConfig, stat);
    }
    result += GetFinalOutputIOMemorySize(ioConfig);
    return result;
}

void TOperationControllerBase::InitIntermediateOutputConfig(TJobIOConfigPtr config)
{
    // Don't replicate intermediate output.
    config->TableWriter->UploadReplicationFactor = Spec_->IntermediateDataReplicationFactor;
    config->TableWriter->MinUploadReplicationFactor = 1;

    // Cache blocks on nodes.
    config->TableWriter->PopulateCache = true;

    // Don't sync intermediate chunks.
    config->TableWriter->SyncOnClose = false;
}

NTableClient::TTableReaderOptionsPtr TOperationControllerBase::CreateTableReaderOptions(TJobIOConfigPtr ioConfig)
{
    auto options = New<TTableReaderOptions>();
    options->EnableRowIndex = ioConfig->ControlAttributes->EnableRowIndex;
    options->EnableTableIndex = ioConfig->ControlAttributes->EnableTableIndex;
    options->EnableRangeIndex = ioConfig->ControlAttributes->EnableRangeIndex;
    return options;
}

void TOperationControllerBase::ValidateUserFileCount(TUserJobSpecPtr spec, const TString& operation)
{
    if (spec && spec->FilePaths.size() > Config->MaxUserFileCount) {
        THROW_ERROR_EXCEPTION("Too many user files in %v: maximum allowed %v, actual %v",
            operation,
            Config->MaxUserFileCount,
            spec->FilePaths.size());
    }
}

void TOperationControllerBase::GetExecNodesInformation()
{
    auto now = NProfiling::GetCpuInstant();
    if (now < GetExecNodesInformationDeadline_) {
        return;
    }

    ExecNodeCount_ = Host->GetExecNodeCount();
    ExecNodesDescriptors_ = Host->GetExecNodeDescriptors(NScheduler::TSchedulingTagFilter(Spec_->SchedulingTagFilter));
    GetExecNodesInformationDeadline_ = now + NProfiling::DurationToCpuDuration(Config->ControllerExecNodeInfoUpdatePeriod);
    LOG_DEBUG("Exec nodes information updated (ExecNodeCount: %v)", ExecNodeCount_);
}

int TOperationControllerBase::GetExecNodeCount()
{
    GetExecNodesInformation();
    return ExecNodeCount_;
}

const TExecNodeDescriptorMap& TOperationControllerBase::GetExecNodeDescriptors()
{
    GetExecNodesInformation();
    return *ExecNodesDescriptors_;
}

bool TOperationControllerBase::ShouldSkipSanityCheck()
{
    auto nodeCount = GetExecNodeCount();
    if (nodeCount < Config->SafeOnlineNodeCount) {
        return true;
    }

    if (TInstant::Now() < Host->GetConnectionTime() + Config->SafeSchedulerOnlineTime) {
        return true;
    }

    if (!CachedMaxAvailableExecNodeResources_) {
        return true;
    }

    return false;
}

void TOperationControllerBase::InferSchemaFromInput(const TKeyColumns& keyColumns)
{
    // We infer schema only for operations with one output table.
    YCHECK(OutputTables_.size() == 1);
    YCHECK(InputTables.size() >= 1);

    OutputTables_[0].TableUploadOptions.SchemaMode = InputTables[0].SchemaMode;
    for (const auto& table : InputTables) {
        if (table.SchemaMode != OutputTables_[0].TableUploadOptions.SchemaMode) {
            THROW_ERROR_EXCEPTION("Cannot infer output schema from input, tables have different schema modes")
                    << TErrorAttribute("input_table1_path", table.GetPath())
                    << TErrorAttribute("input_table1_schema_mode", table.SchemaMode)
                    << TErrorAttribute("input_table2_path", InputTables[0].GetPath())
                    << TErrorAttribute("input_table2_schema_mode", InputTables[0].SchemaMode);
        }
    }

    if (OutputTables_[0].TableUploadOptions.SchemaMode == ETableSchemaMode::Weak) {
        OutputTables_[0].TableUploadOptions.TableSchema = TTableSchema::FromKeyColumns(keyColumns);
    } else {
        auto schema = InputTables[0].Schema
            .ToStrippedColumnAttributes()
            .ToCanonical();

        for (const auto& table : InputTables) {
            if (table.Schema.ToStrippedColumnAttributes().ToCanonical() != schema) {
                THROW_ERROR_EXCEPTION("Cannot infer output schema from input in strong schema mode, tables have incompatible schemas");
            }
        }

        OutputTables_[0].TableUploadOptions.TableSchema = InputTables[0].Schema
            .ToSorted(keyColumns)
            .ToSortedStrippedColumnAttributes()
            .ToCanonical();
    }

    FilterOutputSchemaByInputColumnSelectors();
}

void TOperationControllerBase::InferSchemaFromInputOrdered()
{
    // We infer schema only for operations with one output table.
    YCHECK(OutputTables_.size() == 1);
    YCHECK(InputTables.size() >= 1);

    auto& outputUploadOptions = OutputTables_[0].TableUploadOptions;

    if (InputTables.size() == 1 && outputUploadOptions.UpdateMode == EUpdateMode::Overwrite) {
        // If only only one input table given, we inherit the whole schema including column attributes.
        outputUploadOptions.SchemaMode = InputTables[0].SchemaMode;
        outputUploadOptions.TableSchema = InputTables[0].Schema;
        FilterOutputSchemaByInputColumnSelectors();
        return;
    }

    InferSchemaFromInput();
}

void TOperationControllerBase::FilterOutputSchemaByInputColumnSelectors()
{
    THashSet<TString> columns;
    for (const auto& table : InputTables) {
        if (auto selectors = table.Path.GetColumns()) {
            for (const auto& column : *selectors) {
                columns.insert(column);
            }
        } else {
            return;
        }
    }

    OutputTables_[0].TableUploadOptions.TableSchema =
        OutputTables_[0].TableUploadOptions.TableSchema.Filter(columns);
}

void TOperationControllerBase::ValidateOutputSchemaOrdered() const
{
    YCHECK(OutputTables_.size() == 1);
    YCHECK(InputTables.size() >= 1);

    if (InputTables.size() > 1 && OutputTables_[0].TableUploadOptions.TableSchema.IsSorted()) {
        THROW_ERROR_EXCEPTION("Cannot generate sorted output for ordered operation with multiple input tables")
            << TErrorAttribute("output_schema", OutputTables_[0].TableUploadOptions.TableSchema);
    }
}

void TOperationControllerBase::ValidateOutputSchemaCompatibility(bool ignoreSortOrder, bool validateComputedColumns) const
{
    YCHECK(OutputTables_.size() == 1);

    auto hasComputedColumn = OutputTables_[0].TableUploadOptions.TableSchema.HasComputedColumns();

    for (const auto& inputTable : InputTables) {
        if (inputTable.SchemaMode == ETableSchemaMode::Strong) {
            ValidateTableSchemaCompatibility(
                inputTable.Schema.Filter(inputTable.Path.GetColumns()),
                OutputTables_[0].TableUploadOptions.TableSchema,
                ignoreSortOrder)
                .ThrowOnError();
        } else if (hasComputedColumn && validateComputedColumns) {
            // Input table has weak schema, so we cannot check if all
            // computed columns were already computed. At least this is weird.
            THROW_ERROR_EXCEPTION("Output table cannot have computed "
                "columns, which are not present in all input tables");
        }
    }
}

TJobSplitterConfigPtr TOperationControllerBase::GetJobSplitterConfig() const
{
    return nullptr;
}

void TOperationControllerBase::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, SnapshotIndex_);
    Persist(context, TotalEstimatedInputChunkCount);
    Persist(context, TotalEstimatedInputUncompressedDataSize);
    Persist(context, TotalEstimatedInputRowCount);
    Persist(context, TotalEstimatedInputCompressedDataSize);
    Persist(context, TotalEstimatedInputDataWeight);
    Persist(context, UnavailableInputChunkCount);
    Persist(context, UnavailableIntermediateChunkCount);
    Persist(context, JobCounter);
    Persist(context, InputNodeDirectory_);
    Persist(context, InputTables);
    Persist(context, OutputTables_);
    Persist(context, StderrTable_);
    Persist(context, CoreTable_);
    Persist(context, IntermediateTable);
    Persist<TMapSerializer<TDefaultSerializer, TDefaultSerializer, TUnsortedTag>>(context, UserJobFiles_);
    Persist<TMapSerializer<TDefaultSerializer, TDefaultSerializer, TUnsortedTag>>(context, LivePreviewChunks_);
    Persist(context, Tasks);
    Persist(context, TaskGroups);
    Persist(context, InputChunkMap);
    Persist(context, IntermediateOutputCellTag);
    Persist(context, CellTagToRequiredOutputChunkLists_);
    Persist(context, CellTagToRequiredDebugChunkLists_);
    Persist(context, CachedPendingJobCount);
    Persist(context, CachedNeededResources);
    Persist(context, ChunkOriginMap);
    Persist(context, JobletMap);
    Persist(context, JobIndexGenerator);
    Persist(context, JobStatistics);
    Persist(context, ScheduleJobStatistics_);
    Persist(context, RowCountLimitTableIndex);
    Persist(context, RowCountLimit);
    Persist(context, EstimatedInputDataSizeHistogram_);
    Persist(context, InputDataSizeHistogram_);
    Persist(context, StderrCount_);
    Persist(context, JobNodeCount_);
    Persist(context, FinishedJobs_);
    // COMPAT(max42): remove it in a few weeks.
    if (context.IsLoad()) {
        for (auto& pair : FinishedJobs_) {
            auto& summary = pair.second->Summary;
            summary.StatisticsYson = TYsonString();
            summary.Statistics.Reset();
        }
    }
    Persist(context, JobSpecCompletedArchiveCount_);
    Persist(context, Sinks_);
    Persist(context, AutoMergeTaskGroup);
    Persist(context, AutoMergeTasks);
    Persist(context, AutoMergeJobSpecTemplates_);
    Persist<TUniquePtrSerializer<>>(context, AutoMergeDirector_);
    Persist(context, JobSplitter_);
    Persist(context, DataFlowGraph_);

    // COMPAT(ignat)
    if (context.GetVersion() <= 202001) {
        TYsonString unrecognizedSpecYson("{}");
        if (context.IsSave() && UnrecognizedSpec_) {
            unrecognizedSpecYson = ConvertToYsonString(UnrecognizedSpec_);
        }
        Persist(context, unrecognizedSpecYson);
        if (context.IsLoad()) {
            UnrecognizedSpec_ = ConvertTo<IMapNodePtr>(unrecognizedSpecYson);
        }
    }

    if (context.GetVersion() >= 300003) {
        Persist(context, AvailableExecNodesWereObserved_);
    }

    // NB: Keep this at the end of persist as it requires some of the previous
    // fields to be already intialized.
    if (context.IsLoad()) {
        for (const auto& task : Tasks) {
            task->Initialize();
        }
        InitUpdatingTables();
        InitializeOrchid();
    }
}

void TOperationControllerBase::InitAutoMergeJobSpecTemplates()
{
    // TODO(max42): should this really belong to TOperationControllerBase?
    // We can possibly move it to TAutoMergeTask itself.

    AutoMergeJobSpecTemplates_.resize(OutputTables_.size());
    for (int tableIndex = 0; tableIndex < OutputTables_.size(); ++tableIndex) {
        AutoMergeJobSpecTemplates_[tableIndex].set_type(static_cast<int>(EJobType::UnorderedMerge));
        auto* schedulerJobSpecExt = AutoMergeJobSpecTemplates_[tableIndex]
            .MutableExtension(TSchedulerJobSpecExt::scheduler_job_spec_ext);
        schedulerJobSpecExt->set_table_reader_options(
            ConvertToYsonString(CreateTableReaderOptions(Spec_->AutoMerge->JobIO)).GetData());

        auto dataSourceDirectory = New<TDataSourceDirectory>();
        // NB: chunks read by auto-merge jobs have table index set to output table index,
        // so we need to specify several unused data sources before actual one.
        dataSourceDirectory->DataSources().resize(tableIndex);
        dataSourceDirectory->DataSources().push_back(MakeUnversionedDataSource(
            IntermediatePath,
            OutputTables_[tableIndex].TableUploadOptions.TableSchema,
            Null));

        NChunkClient::NProto::TDataSourceDirectoryExt dataSourceDirectoryExt;
        ToProto(&dataSourceDirectoryExt, dataSourceDirectory);
        SetProtoExtension(schedulerJobSpecExt->mutable_extensions(), dataSourceDirectoryExt);
        schedulerJobSpecExt->set_io_config(ConvertToYsonString(Spec_->AutoMerge->JobIO).GetData());
    }
}

void TOperationControllerBase::ValidateRevivalAllowed() const
{
    if (Spec_->FailOnJobRestart) {
        THROW_ERROR_EXCEPTION(
            NScheduler::EErrorCode::OperationFailedOnJobRestart,
            "Cannot revive operation when spec option fail_on_job_restart is set")
                << TErrorAttribute("operation_type", OperationType);
    }
}

void TOperationControllerBase::ValidateSnapshot() const
{ }

std::vector<TUserJobSpecPtr> TOperationControllerBase::GetUserJobSpecs() const
{
    return {};
}

EIntermediateChunkUnstageMode TOperationControllerBase::GetIntermediateChunkUnstageMode() const
{
    return EIntermediateChunkUnstageMode::OnSnapshotCompleted;
}

TBlobTableWriterConfigPtr TOperationControllerBase::GetStderrTableWriterConfig() const
{
    return nullptr;
}

TNullable<TRichYPath> TOperationControllerBase::GetStderrTablePath() const
{
    return Null;
}

TBlobTableWriterConfigPtr TOperationControllerBase::GetCoreTableWriterConfig() const
{
    return nullptr;
}

TNullable<TRichYPath> TOperationControllerBase::GetCoreTablePath() const
{
    return Null;
}

void TOperationControllerBase::OnChunksReleased(int /* chunkCount */)
{ }

TTableWriterOptionsPtr TOperationControllerBase::GetIntermediateTableWriterOptions() const
{
    auto options = New<NTableClient::TTableWriterOptions>();
    options->Account = Spec_->IntermediateDataAccount;
    options->ChunksVital = false;
    options->ChunksMovable = false;
    options->ReplicationFactor = Spec_->IntermediateDataReplicationFactor;
    options->MediumName = Spec_->IntermediateDataMediumName;
    options->CompressionCodec = Spec_->IntermediateCompressionCodec;
    // Distribute intermediate chunks uniformly across storage locations.
    options->PlacementId = GetOperationId();
    options->TableIndex = 0;
    return options;
}

TEdgeDescriptor TOperationControllerBase::GetIntermediateEdgeDescriptorTemplate() const
{
    TEdgeDescriptor descriptor;
    descriptor.CellTag = GetIntermediateOutputCellTag();
    descriptor.TableWriterOptions = GetIntermediateTableWriterOptions();
    descriptor.RequiresRecoveryInfo = true;
    return descriptor;
}

void TOperationControllerBase::ReleaseIntermediateStripeList(const NChunkPools::TChunkStripeListPtr& stripeList)
{
    switch (GetIntermediateChunkUnstageMode()) {
        case EIntermediateChunkUnstageMode::OnJobCompleted: {
            auto chunks = GetStripeListChunks(stripeList);
            AddChunksToUnstageList(std::move(chunks));
            OnChunksReleased(stripeList->TotalChunkCount);
            break;
        }
        case EIntermediateChunkUnstageMode::OnSnapshotCompleted: {
            IntermediateStripeListReleaseQueue_.Push(stripeList);
            break;
        }
        default:
            Y_UNREACHABLE();
    }
}

TDataFlowGraph* TOperationControllerBase::GetDataFlowGraph()
{
    return DataFlowGraph_.Get();
}

void TOperationControllerBase::TLivePreviewChunkDescriptor::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, VertexDescriptor);
    Persist(context, LivePreviewIndex);
}

void TOperationControllerBase::RegisterLivePreviewChunk(
    const TDataFlowGraph::TVertexDescriptor& vertexDescriptor,
    int index,
    const TInputChunkPtr& chunk)
{
    YCHECK(LivePreviewChunks_.insert(std::make_pair(
        chunk,
        TLivePreviewChunkDescriptor{vertexDescriptor, index})).second);

    DataFlowGraph_->RegisterLivePreviewChunk(vertexDescriptor, index, chunk);
}

const IThroughputThrottlerPtr& TOperationControllerBase::GetJobSpecSliceThrottler() const
{
    return Host->GetJobSpecSliceThrottler();
}

void TOperationControllerBase::FinishTaskInput(const TTaskPtr& task)
{
    task->FinishInput(TDataFlowGraph::SourceDescriptor);
}

void TOperationControllerBase::SetOperationAlert(EOperationAlertType type, const TError& alert)
{
    TGuard<TSpinLock> guard(AlertsLock_);
    Alerts_[type] = alert;
}

bool TOperationControllerBase::IsCompleted() const
{
    for (const auto& task : AutoMergeTasks) {
        if (task && !task->IsCompleted()) {
            return false;
        }
    }
    return true;
}

////////////////////////////////////////////////////////////////////////////////

TOperationControllerBase::TSink::TSink(TOperationControllerBase* controller, int outputTableIndex)
    : Controller_(controller)
    , OutputTableIndex_(outputTableIndex)
{ }

IChunkPoolInput::TCookie TOperationControllerBase::TSink::AddWithKey(TChunkStripePtr stripe, TChunkStripeKey key)
{
    YCHECK(stripe->ChunkListId);
    auto& table = Controller_->OutputTables_[OutputTableIndex_];
    auto chunkListId = stripe->ChunkListId;

    if (table.TableUploadOptions.TableSchema.IsSorted() && Controller_->ShouldVerifySortedOutput()) {
        // We override the key suggested by the task with the one formed by the stripe boundary keys.
        YCHECK(stripe->BoundaryKeys);
        key = stripe->BoundaryKeys;
    }

    if (Controller_->IsOutputLivePreviewSupported()) {
        Controller_->AttachToLivePreview(chunkListId, table.LivePreviewTableId);
    }
    table.OutputChunkTreeIds.emplace_back(key, chunkListId);

    const auto& Logger = Controller_->Logger;
    LOG_DEBUG("Output stripe registered (Table: %v, ChunkListId: %v, Key: %v)",
        OutputTableIndex_,
        chunkListId,
        key);

    return IChunkPoolInput::NullCookie;
}

IChunkPoolInput::TCookie TOperationControllerBase::TSink::Add(TChunkStripePtr stripe)
{
    return AddWithKey(stripe, TChunkStripeKey());
}

void TOperationControllerBase::TSink::Suspend(TCookie /* cookie */)
{
    Y_UNREACHABLE();
}

void TOperationControllerBase::TSink::Resume(TCookie /* cookie */)
{
    Y_UNREACHABLE();
}

void TOperationControllerBase::TSink::Reset(
    TCookie /* cookie */,
    TChunkStripePtr /* stripe */,
    TInputChunkMappingPtr /* mapping */)
{
    Y_UNREACHABLE();
}

void TOperationControllerBase::TSink::Finish()
{
    // Mmkay. Don't know what to do here though :)
}

void TOperationControllerBase::TSink::Persist(const TPersistenceContext& context)
{
    using NYT::Persist;

    Persist(context, Controller_);
    Persist(context, OutputTableIndex_);
}

DEFINE_DYNAMIC_PHOENIX_TYPE(TOperationControllerBase::TSink);

////////////////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT
