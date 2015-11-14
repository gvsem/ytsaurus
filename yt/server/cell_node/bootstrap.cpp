#include "bootstrap.h"
#include "config.h"

#include <yt/server/data_node/blob_reader_cache.h>
#include <yt/server/data_node/block_cache.h>
#include <yt/server/data_node/chunk_block_manager.h>
#include <yt/server/data_node/chunk_cache.h>
#include <yt/server/data_node/chunk_registry.h>
#include <yt/server/data_node/chunk_store.h>
#include <yt/server/data_node/config.h>
#include <yt/server/data_node/data_node_service.h>
#include <yt/server/data_node/job.h>
#include <yt/server/data_node/journal_dispatcher.h>
#include <yt/server/data_node/location.h>
#include <yt/server/data_node/master_connector.h>
#include <yt/server/data_node/peer_block_table.h>
#include <yt/server/data_node/peer_block_updater.h>
#include <yt/server/data_node/private.h>
#include <yt/server/data_node/session_manager.h>
#include <yt/server/data_node/ytree_integration.h>
#include <yt/server/data_node/chunk_meta_manager.h>

#include <yt/server/exec_agent/config.h>
#include <yt/server/exec_agent/environment.h>
#include <yt/server/exec_agent/environment_manager.h>
#include <yt/server/exec_agent/job.h>
#include <yt/server/exec_agent/job_prober_service.h>
#include <yt/server/exec_agent/private.h>
#include <yt/server/exec_agent/scheduler_connector.h>
#include <yt/server/exec_agent/slot_manager.h>
#include <yt/server/exec_agent/supervisor_service.h>
#include <yt/server/exec_agent/unsafe_environment.h>

#include <yt/server/job_agent/job_controller.h>

#include <yt/server/misc/build_attributes.h>
#include <yt/server/misc/memory_usage_tracker.h>

#include <yt/server/object_server/master_cache_service.h>

#include <yt/server/query_agent/query_executor.h>
#include <yt/server/query_agent/query_service.h>

#include <yt/server/tablet_node/in_memory_manager.h>
#include <yt/server/tablet_node/partition_balancer.h>
#include <yt/server/tablet_node/security_manager.h>
#include <yt/server/tablet_node/slot_manager.h>
#include <yt/server/tablet_node/store_compactor.h>
#include <yt/server/tablet_node/store_flusher.h>

#include <yt/server/transaction_server/timestamp_proxy_service.h>

#include <yt/ytlib/api/client.h>
#include <yt/ytlib/api/connection.h>

#include <yt/ytlib/chunk_client/chunk_service_proxy.h>
#include <yt/ytlib/chunk_client/client_block_cache.h>

#include <yt/ytlib/hydra/peer_channel.h>

#include <yt/ytlib/misc/workload.h>

#include <yt/ytlib/monitoring/http_integration.h>
#include <yt/ytlib/monitoring/http_server.h>
#include <yt/ytlib/monitoring/monitoring_manager.h>

#include <yt/ytlib/object_client/helpers.h>
#include <yt/ytlib/object_client/object_service_proxy.h>

#include <yt/ytlib/orchid/orchid_service.h>

#include <yt/core/bus/config.h>
#include <yt/core/bus/server.h>
#include <yt/core/bus/tcp_server.h>

#include <yt/core/concurrency/action_queue.h>

#include <yt/core/misc/address.h>
#include <yt/core/misc/collection_helpers.h>
#include <yt/core/misc/common.h>
#include <yt/core/misc/ref_counted_tracker.h>

#include <yt/core/profiling/profile_manager.h>

#include <yt/core/rpc/bus_channel.h>
#include <yt/core/rpc/bus_server.h>
#include <yt/core/rpc/caching_channel_factory.h>
#include <yt/core/rpc/channel.h>
#include <yt/core/rpc/redirector_service.h>
#include <yt/core/rpc/server.h>
#include <yt/core/rpc/throttling_channel.h>

#include <yt/core/ytree/ephemeral_node_factory.h>
#include <yt/core/ytree/virtual.h>
#include <yt/core/ytree/ypath_client.h>

namespace NYT {
namespace NCellNode {

using namespace NBus;
using namespace NObjectClient;
using namespace NChunkClient;
using namespace NNodeTrackerClient;
using namespace NChunkServer;
using namespace NElection;
using namespace NHydra;
using namespace NMonitoring;
using namespace NOrchid;
using namespace NProfiling;
using namespace NRpc;
using namespace NYTree;
using namespace NConcurrency;
using namespace NScheduler;
using namespace NJobAgent;
using namespace NExecAgent;
using namespace NJobProxy;
using namespace NDataNode;
using namespace NTabletNode;
using namespace NQueryAgent;
using namespace NApi;
using namespace NTransactionServer;

////////////////////////////////////////////////////////////////////////////////

static NLogging::TLogger Logger("Bootstrap");
static const i64 FootprintMemorySize = (i64) 1024 * 1024 * 1024;

////////////////////////////////////////////////////////////////////////////////

TBootstrap::TBootstrap(INodePtr configNode)
    : ConfigNode(configNode)
{ }

TBootstrap::~TBootstrap()
{ }

void TBootstrap::Run()
{
    srand(time(nullptr));

    ControlQueue = New<TActionQueue>("Control");

    BIND(&TBootstrap::DoRun, this)
        .AsyncVia(GetControlInvoker())
        .Run()
        .Get()
        .ThrowOnError();

    Sleep(TDuration::Max());
}

void TBootstrap::DoRun()
{
    try {
        Config = ConvertTo<TCellNodeConfigPtr>(ConfigNode);
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Error parsing cell node configuration")
            << ex;
    }

    auto localAddresses = GetLocalAddresses();

    LOG_INFO("Starting node (LocalAddresses: [%v], MasterAddresses: [%v])",
        JoinToString(GetValues(localAddresses)),
        JoinToString(Config->ClusterConnection->Master->Addresses));

    MemoryUsageTracker = std::make_unique<TNodeMemoryTracker>(
        Config->ResourceLimits->Memory,
        std::vector<std::pair<EMemoryCategory, i64>>{
            {EMemoryCategory::Jobs, Config->ExecAgent->JobController->ResourceLimits->Memory},
            {EMemoryCategory::TabletStatic, Config->TabletNode->ResourceLimits->TabletStaticMemory },
            {EMemoryCategory::TabletDynamic, Config->TabletNode->ResourceLimits->TabletDynamicMemory }
        },
        Logger,
        TProfiler("/cell_node/memory_usage"));

    {
        auto result = MemoryUsageTracker->TryAcquire(EMemoryCategory::Footprint, FootprintMemorySize);
        THROW_ERROR_EXCEPTION_IF_FAILED(result, "Error reserving footprint memory");
    }

    auto clusterConnection = CreateConnection(Config->ClusterConnection);
    MasterClient = clusterConnection->CreateClient(GetRootClientOptions());

    QueryThreadPool = New<TThreadPool>(
        Config->QueryAgent->ThreadPoolSize,
        "Query");

    BusServer = CreateTcpBusServer(TTcpBusServerConfig::CreateTcp(Config->RpcPort));

    RpcServer = CreateBusServer(BusServer);

    HttpServer.reset(new NHttp::TServer(Config->MonitoringPort));

    TabletChannelFactory = CreateCachingChannelFactory(GetBusChannelFactory());

    auto monitoringManager = New<TMonitoringManager>();
    monitoringManager->Register(
        "/ref_counted",
        TRefCountedTracker::Get()->GetMonitoringProducer());

    // NB: No retries, no user overriding.
    auto directMasterChannel = CreatePeerChannel(
        Config->ClusterConnection->Master,
        GetBusChannelFactory(),
        EPeerKind::Leader);

    auto masterRedirectorChannel = CreateThrottlingChannel(
        Config->MasterRedirectorService,
        directMasterChannel);
    auto redirectorCellId = ToRedirectorCellId(GetCellId());
    RpcServer->RegisterService(CreateRedirectorService(
        TServiceId(NChunkClient::TChunkServiceProxy::GetServiceName(), redirectorCellId),
        masterRedirectorChannel));
    RpcServer->RegisterService(CreateRedirectorService(
        TServiceId(NObjectClient::TObjectServiceProxy::GetServiceName(), redirectorCellId),
        masterRedirectorChannel));

    BlobReaderCache = New<TBlobReaderCache>(Config->DataNode);

    JournalDispatcher = New<TJournalDispatcher>(Config->DataNode);

    ChunkRegistry = New<TChunkRegistry>(this);

    ChunkMetaManager = New<TChunkMetaManager>(Config->DataNode, this);

    ChunkBlockManager = New<TChunkBlockManager>(Config->DataNode, this);

    BlockCache = CreateServerBlockCache(Config->DataNode, this);

    PeerBlockTable = New<TPeerBlockTable>(Config->DataNode->PeerBlockTable);

    PeerBlockUpdater = New<TPeerBlockUpdater>(Config->DataNode, this);

    SessionManager = New<TSessionManager>(Config->DataNode, this);

    MasterConnector = New<NDataNode::TMasterConnector>(
        Config->DataNode,
        localAddresses,
        this);

    MasterConnector->SubscribePopulateAlerts(BIND(&TBootstrap::PopulateAlerts, this));

    ChunkStore = New<NDataNode::TChunkStore>(Config->DataNode, this);

    ChunkCache = New<TChunkCache>(Config->DataNode, this);

    auto createThrottler = [] (TThroughputThrottlerConfigPtr config, const Stroka& name) -> IThroughputThrottlerPtr {
        auto logger = DataNodeLogger;
        logger.AddTag("Throttler: %v", name);

        auto profiler = NProfiling::TProfiler(
            DataNodeProfiler.GetPathPrefix() + "/" +
            CamelCaseToUnderscoreCase(name));

        return CreateLimitedThrottler(config, logger, profiler);
    };
    ReplicationInThrottler = createThrottler(Config->DataNode->ReplicationInThrottler, "ReplicationIn");
    ReplicationOutThrottler = createThrottler(Config->DataNode->ReplicationOutThrottler, "ReplicationOut");
    RepairInThrottler = createThrottler(Config->DataNode->RepairInThrottler, "RepairIn");
    RepairOutThrottler = createThrottler(Config->DataNode->RepairOutThrottler, "RepairOut");

    RpcServer->RegisterService(CreateDataNodeService(Config->DataNode, this));

    JobProxyConfig = New<NJobProxy::TJobProxyConfig>();
    
    JobProxyConfig->ClusterConnection = CloneYsonSerializable(Config->ClusterConnection);
    JobProxyConfig->ClusterConnection->Master->Addresses = {GetInterconnectAddress(localAddresses)};
    JobProxyConfig->ClusterConnection->Master->CellId = redirectorCellId;

    JobProxyConfig->MemoryWatchdogPeriod = Config->ExecAgent->MemoryWatchdogPeriod;
    JobProxyConfig->BlockIOWatchdogPeriod = Config->ExecAgent->BlockIOWatchdogPeriod;

    JobProxyConfig->Logging = Config->ExecAgent->JobProxyLogging;
    JobProxyConfig->Tracing = Config->ExecAgent->JobProxyTracing;

    JobProxyConfig->MemoryLimitMultiplier = Config->ExecAgent->MemoryLimitMultiplier;

    JobProxyConfig->EnableCGroups = Config->ExecAgent->EnableCGroups;
    JobProxyConfig->SupportedCGroups = Config->ExecAgent->SupportedCGroups;

    JobProxyConfig->EnableIopsThrottling = Config->ExecAgent->EnableIopsThrottling;

    JobProxyConfig->AddressResolver = Config->AddressResolver;
    JobProxyConfig->SupervisorConnection = New<NBus::TTcpBusClientConfig>();
    JobProxyConfig->SupervisorConnection->Address = GetInterconnectAddress(localAddresses);
    JobProxyConfig->SupervisorRpcTimeout = Config->ExecAgent->SupervisorRpcTimeout;
    // TODO(babenko): consider making this priority configurable
    JobProxyConfig->SupervisorConnection->Priority = 6;

    ExecSlotManager = New<NExecAgent::TSlotManager>(Config->ExecAgent->SlotManager, this);

    JobController = New<TJobController>(Config->ExecAgent->JobController, this);

    auto createExecJob = BIND([this] (
            const NJobAgent::TJobId& jobId,
            const NNodeTrackerClient::NProto::TNodeResources& resourceLimits,
            NJobTrackerClient::NProto::TJobSpec&& jobSpec) ->
            NJobAgent::IJobPtr
        {
            return NExecAgent::CreateUserJob(
                    jobId,
                    resourceLimits,
                    std::move(jobSpec),
                    this);
        });
    JobController->RegisterFactory(NJobAgent::EJobType::Map,             createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::PartitionMap,    createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SortedMerge,     createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::OrderedMerge,    createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::UnorderedMerge,  createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::Partition,       createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SimpleSort,      createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::IntermediateSort,createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::FinalSort,       createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SortedReduce,    createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::PartitionReduce, createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::ReduceCombiner,  createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::RemoteCopy,      createExecJob);
    JobController->RegisterFactory(NJobAgent::EJobType::OrderedMap,      createExecJob);

    auto createChunkJob = BIND([this] (
            const NJobAgent::TJobId& jobId,
            const NNodeTrackerClient::NProto::TNodeResources& resourceLimits,
            NJobTrackerClient::NProto::TJobSpec&& jobSpec) ->
            NJobAgent::IJobPtr
        {
            return NDataNode::CreateChunkJob(
                    jobId,
                    std::move(jobSpec),
                    resourceLimits,
                    Config->DataNode,
                    this);
        });
    JobController->RegisterFactory(NJobAgent::EJobType::RemoveChunk,     createChunkJob);
    JobController->RegisterFactory(NJobAgent::EJobType::ReplicateChunk,  createChunkJob);
    JobController->RegisterFactory(NJobAgent::EJobType::RepairChunk,     createChunkJob);
    JobController->RegisterFactory(NJobAgent::EJobType::SealChunk,       createChunkJob);

    RpcServer->RegisterService(CreateJobProberService(this));

    RpcServer->RegisterService(New<TSupervisorService>(this));

    EnvironmentManager = New<TEnvironmentManager>(Config->ExecAgent->EnvironmentManager);
    EnvironmentManager->Register("unsafe", CreateUnsafeEnvironmentBuilder());

    SchedulerConnector = New<TSchedulerConnector>(Config->ExecAgent->SchedulerConnector, this);

    TabletSlotManager = New<NTabletNode::TSlotManager>(Config->TabletNode, this);

    SecurityManager = New<TSecurityManager>(Config->TabletNode->SecurityManager, this);

    InMemoryManager = New<TInMemoryManager>(Config->TabletNode->InMemoryManager, this);

    QueryExecutor = CreateQueryExecutor(Config->QueryAgent, this);

    RpcServer->RegisterService(CreateQueryService(Config->QueryAgent, this));

    RpcServer->RegisterService(CreateTimestampProxyService(
        clusterConnection->GetTimestampProvider()));

    RpcServer->RegisterService(CreateMasterCacheService(
        Config->MasterCacheService,
        directMasterChannel,
        GetCellId()));

    OrchidRoot = GetEphemeralNodeFactory()->CreateMap();
    SetNodeByYPath(
        OrchidRoot,
        "/monitoring",
        CreateVirtualNode(monitoringManager->GetService()));
    SetNodeByYPath(
        OrchidRoot,
        "/profiling",
        CreateVirtualNode(TProfileManager::Get()->GetService()));
    SetNodeByYPath(
        OrchidRoot,
        "/config",
        ConfigNode);
    SetNodeByYPath(
        OrchidRoot,
        "/stored_chunks",
        CreateVirtualNode(CreateStoredChunkMapService(ChunkStore)
            ->Via(GetControlInvoker())));
    SetNodeByYPath(
        OrchidRoot,
        "/cached_chunks",
        CreateVirtualNode(CreateCachedChunkMapService(ChunkCache)
            ->Via(GetControlInvoker())));
    SetNodeByYPath(
        OrchidRoot,
        "/tablet_cells",
        CreateVirtualNode(
            TabletSlotManager->GetOrchidService()
            ->Via(GetControlInvoker())
            ->Cached(Config->OrchidCacheUpdatePeriod)));
    SetBuildAttributes(OrchidRoot, "node");

    HttpServer->Register(
        "/orchid",
        NMonitoring::GetYPathHttpHandler(OrchidRoot->Via(GetControlInvoker())));

    RpcServer->RegisterService(CreateOrchidService(
        OrchidRoot,
        GetControlInvoker()));

    LOG_INFO("Listening for HTTP requests on port %v", Config->MonitoringPort);

    LOG_INFO("Listening for RPC requests on port %v", Config->RpcPort);
    RpcServer->Configure(Config->RpcServer);

    // Do not start subsystems until everything is initialized.
    TabletSlotManager->Initialize();
    ChunkStore->Initialize();
    ChunkCache->Initialize();
    ExecSlotManager->Initialize(Config->ExecAgent->JobController->ResourceLimits->UserSlots);
    monitoringManager->Start();
    PeerBlockUpdater->Start();
    MasterConnector->Start();
    SchedulerConnector->Start();
    StartStoreFlusher(Config->TabletNode, this);
    StartStoreCompactor(Config->TabletNode, this);
    StartPartitionBalancer(Config->TabletNode->PartitionBalancer, this);

    RpcServer->Start();
    HttpServer->Start();
}

TCellNodeConfigPtr TBootstrap::GetConfig() const
{
    return Config;
}

IInvokerPtr TBootstrap::GetControlInvoker() const
{
    return ControlQueue->GetInvoker();
}

IInvokerPtr TBootstrap::GetQueryPoolInvoker() const
{
    return QueryThreadPool->GetInvoker();
}

IClientPtr TBootstrap::GetMasterClient() const
{
    return MasterClient;
}

IServerPtr TBootstrap::GetRpcServer() const
{
    return RpcServer;
}

IChannelFactoryPtr TBootstrap::GetTabletChannelFactory() const
{
    return TabletChannelFactory;
}

IMapNodePtr TBootstrap::GetOrchidRoot() const
{
    return OrchidRoot;
}

TJobControllerPtr TBootstrap::GetJobController() const
{
    return JobController;
}

NTabletNode::TSlotManagerPtr TBootstrap::GetTabletSlotManager() const
{
    return TabletSlotManager;
}

TSecurityManagerPtr TBootstrap::GetSecurityManager() const
{
    return SecurityManager;
}

TInMemoryManagerPtr TBootstrap::GetInMemoryManager() const
{
    return InMemoryManager;
}

NExecAgent::TSlotManagerPtr TBootstrap::GetExecSlotManager() const
{
    return ExecSlotManager;
}

TEnvironmentManagerPtr TBootstrap::GetEnvironmentManager() const
{
    return EnvironmentManager;
}

TJobProxyConfigPtr TBootstrap::GetJobProxyConfig() const
{
    return JobProxyConfig;
}

NDataNode::TChunkStorePtr TBootstrap::GetChunkStore() const
{
    return ChunkStore;
}

TChunkCachePtr TBootstrap::GetChunkCache() const
{
    return ChunkCache;
}

TNodeMemoryTracker* TBootstrap::GetMemoryUsageTracker() const
{
    return MemoryUsageTracker.get();
}

TChunkRegistryPtr TBootstrap::GetChunkRegistry() const
{
    return ChunkRegistry;
}

TSessionManagerPtr TBootstrap::GetSessionManager() const
{
    return SessionManager;
}

TChunkBlockManagerPtr TBootstrap::GetChunkBlockManager() const
{
    return ChunkBlockManager;
}

TChunkMetaManagerPtr TBootstrap::GetChunkMetaManager() const
{
    return ChunkMetaManager;
}

IBlockCachePtr TBootstrap::GetBlockCache() const
{
    return BlockCache;
}

TPeerBlockTablePtr TBootstrap::GetPeerBlockTable() const
{
    return PeerBlockTable;
}

TBlobReaderCachePtr TBootstrap::GetBlobReaderCache() const
{
    return BlobReaderCache;
}

TJournalDispatcherPtr TBootstrap::GetJournalDispatcher() const
{
    return JournalDispatcher;
}

NDataNode::TMasterConnectorPtr TBootstrap::GetMasterConnector() const
{
    return MasterConnector;
}

NQueryClient::ISubExecutorPtr TBootstrap::GetQueryExecutor() const
{
    return QueryExecutor;
}

const TCellId& TBootstrap::GetCellId() const
{
    return Config->ClusterConnection->Master->CellId;
}

IThroughputThrottlerPtr TBootstrap::GetReplicationInThrottler() const
{
    return ReplicationInThrottler;
}

IThroughputThrottlerPtr TBootstrap::GetReplicationOutThrottler() const
{
    return ReplicationOutThrottler;
}

IThroughputThrottlerPtr TBootstrap::GetRepairInThrottler() const
{
    return RepairInThrottler;
}

IThroughputThrottlerPtr TBootstrap::GetRepairOutThrottler() const
{
    return RepairOutThrottler;
}

IThroughputThrottlerPtr TBootstrap::GetInThrottler(const TWorkloadDescriptor& descriptor) const
{
    switch (descriptor.Category) {
        case EWorkloadCategory::SystemRepair:
            return RepairInThrottler;

        case EWorkloadCategory::SystemReplication:
            return ReplicationInThrottler;

        default:
            return GetUnlimitedThrottler();
    }
}

IThroughputThrottlerPtr TBootstrap::GetOutThrottler(const TWorkloadDescriptor& descriptor) const
{
    switch (descriptor.Category) {
        case EWorkloadCategory::SystemRepair:
            return RepairOutThrottler;

        case EWorkloadCategory::SystemReplication:
            return ReplicationOutThrottler;

        default:
            return GetUnlimitedThrottler();
    }
}

TAddressMap TBootstrap::GetLocalAddresses()
{
    // First without port number.
    auto hostNames = Config->Addresses;
    if (hostNames.find(NNodeTrackerClient::DefaultNetworkName) == hostNames.end()) {
        YCHECK(hostNames.insert(std::make_pair(
            NNodeTrackerClient::DefaultNetworkName,
            TAddressResolver::Get()->GetLocalHostName())).second);
    }

    // Now append port number.
    TAddressMap addresses;
    for (auto& pair : hostNames) {
        YCHECK(addresses.insert(std::make_pair(
            pair.first,
            BuildServiceAddress(pair.second, Config->RpcPort))).second);
    }

    return addresses;
}

void TBootstrap::PopulateAlerts(std::vector<TError>* alerts)
{
    // NB: Don't expect IsXXXExceeded helpers to be atomic.
    auto totalUsed = MemoryUsageTracker->GetTotalUsed();
    auto totalLimit = MemoryUsageTracker->GetTotalLimit();
    if (totalUsed > totalLimit) {
        alerts->push_back(TError("Total memory limit exceeded")
            << TErrorAttribute("used", totalUsed)
            << TErrorAttribute("limit", totalLimit));
    }

    for (auto category : TEnumTraits<EMemoryCategory>::GetDomainValues()) {
        auto used = MemoryUsageTracker->GetUsed(category);
        auto limit = MemoryUsageTracker->GetLimit(category);
        if (used > limit) {
            alerts->push_back(TError("Memory limit exceeded for category %Qlv",
                category)
                << TErrorAttribute("used", used)
                << TErrorAttribute("limit", limit));
        }
    }
}

TCellId TBootstrap::ToRedirectorCellId(const TCellId& cellId)
{
    return ReplaceCellTagInId(
        TCellId(0xffffffffULL, 0xffffffffULL),
        CellTagFromId(cellId));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCellNode
} // namespace NYT
