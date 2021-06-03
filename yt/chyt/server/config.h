#pragma once

#include "private.h"

#include <yt/yt/server/lib/misc/config.h>

#include <yt/yt/ytlib/api/native/config.h>

#include <yt/yt/ytlib/chunk_client/config.h>

#include <yt/yt/ytlib/table_client/config.h>

#include <yt/yt/ytlib/object_client/config.h>

#include <yt/yt/ytlib/security_client/config.h>

#include <yt/yt/client/misc/config.h>

#include <yt/yt/client/ypath/rich.h>

#include <yt/yt/client/table_client/config.h>

#include <yt/yt/core/concurrency/config.h>

#include <yt/yt/core/ytree/fluent.h>

#include <yt/yt/core/yson/public.h>

namespace NYT::NClickHouseServer {

////////////////////////////////////////////////////////////////////////////////

//! Settings affecting how CHYT behaves around composite values and any columns.
class TCompositeSettings
    : public NYTree::TYsonSerializable
{
public:
    EExtendedYsonFormat DefaultYsonFormat;

    TCompositeSettings();
};

DEFINE_REFCOUNTED_TYPE(TCompositeSettings);

////////////////////////////////////////////////////////////////////////////////

class TDynamicTableSettings
    : public NYTree::TYsonSerializable
{
public:
    bool EnableDynamicStoreRead;

    int WriteRetryCount;

    TDuration WriteRetryBackoff;

    int MaxRowsPerWrite;

    NTransactionClient::EAtomicity TransactionAtomicity;

    bool FetchFromTablets;

    TDynamicTableSettings();
};

DEFINE_REFCOUNTED_TYPE(TDynamicTableSettings);

////////////////////////////////////////////////////////////////////////////////

class TTestingSettings
    : public NYTree::TYsonSerializable
{
public:
    bool EnableKeyConditionFiltering;
    bool MakeUpperBoundInclusive;

    bool ThrowExceptionInDistributor;
    bool ThrowExceptionInSubquery;
    i64 SubqueryAllocationSize;

    bool HangControlInvoker;

    TTestingSettings();
};

DEFINE_REFCOUNTED_TYPE(TTestingSettings);

////////////////////////////////////////////////////////////////////////////////

class TExecutionSettings
    : public NYTree::TYsonSerializable
{
public:
    //! Hard limit. Query will be aborted after reaching this.
    //! -1 means unlimited.
    i64 QueryDepthLimit;
    // i64 TotalSecondaryQueryLimit;

    //! Do not distribute small tables!
    i64 MinDataWeightPerSecondaryQuery;

    //! -1 means unlimited.
    i64 DistributedJoinNodeLimit;
    //! -1 means unlimited.
    i64 DistributedSelectNodeLimit;

    //! Soft limits. After reaching, queries will be processed with local node only.
    //! -1 means unlimited.
    i64 DistributedJoinDepthLimit;
    //! -1 means unlimited.
    i64 DistributedSelectDepthLimit;

    EJoinPolicy JoinPolicy;

    //! To distribute queries deterministically.
    size_t DistributionSeed;

    //! if 0, the max_threads is used.
    i64 InputStreamsPerSecondaryQuery;

    TExecutionSettings();
};

DEFINE_REFCOUNTED_TYPE(TExecutionSettings);

////////////////////////////////////////////////////////////////////////////////

//! This class will be accessible either via settings or via default_settings.
class TQuerySettings
    : public NYTree::TYsonSerializable
{
public:
    bool EnableColumnarRead;

    bool EnableComputedColumnDeduction;

    bool UseBlockSampling;

    EDeducedStatementMode DeducedStatementMode;

    bool LogKeyConditionDetails;

    bool ConvertRowBatchesInWorkerThreadPool;

    bool InferDynamicTableRangesFromPivotKeys;

    TCompositeSettingsPtr Composite;

    TDynamicTableSettingsPtr DynamicTable;

    TTestingSettingsPtr Testing;

    TExecutionSettingsPtr Execution;

    NTableClient::TTableReaderConfigPtr TableReader;

    bool EnableReaderTracing;

    TQuerySettings();
};

DEFINE_REFCOUNTED_TYPE(TQuerySettings)

////////////////////////////////////////////////////////////////////////////////

class THealthCheckerConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration Period;
    TDuration Timeout;
    std::vector<TString> Queries;

    THealthCheckerConfig();
};

DEFINE_REFCOUNTED_TYPE(THealthCheckerConfig)

////////////////////////////////////////////////////////////////////////////////

class TShowTablesConfig
    : public NYTree::TYsonSerializable
{
public:
    std::vector<TString> Roots;

    TShowTablesConfig();
};

DEFINE_REFCOUNTED_TYPE(TShowTablesConfig)

////////////////////////////////////////////////////////////////////////////////

class TSubqueryConfig
    : public NYTree::TYsonSerializable
{
public:
    NChunkClient::TFetcherConfigPtr ChunkSliceFetcher;
    int MaxJobCountForPool;
    int MinDataWeightPerThread;

    // Two fields below are for the chunk spec fetcher.
    int MaxChunksPerFetch;
    int MaxChunksPerLocateRequest;

    i64 MaxDataWeightPerSubquery;
    bool UseColumnarStatistics;

    i64 MinSliceDataWeight;

    TSubqueryConfig();
};

DEFINE_REFCOUNTED_TYPE(TSubqueryConfig)

////////////////////////////////////////////////////////////////////////////////

class TMemoryWatchdogConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Memory limit for the job.
    size_t MemoryLimit;

    //! If remaining memory becomes less than `CodicilWatermark`, process dumps its query registry
    //! to simplify the investigation of its inevitable^W possible death.
    size_t CodicilWatermark;

    TDuration WindowWidth;
    //! If remaining memory is does not exceed #WindowCodicilWatermark for #WindowWitdth time,
    //! dump process query registry and die.
    size_t WindowCodicilWatermark;

    //! Check period.
    TDuration Period;

    TMemoryWatchdogConfig();
};

DEFINE_REFCOUNTED_TYPE(TMemoryWatchdogConfig);

////////////////////////////////////////////////////////////////////////////////

class TSecurityManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    bool Enable;

    TDuration OperationAclUpdatePeriod;

    TSecurityManagerConfig();
};

DEFINE_REFCOUNTED_TYPE(TSecurityManagerConfig);

////////////////////////////////////////////////////////////////////////////////

class TQueryStatisticsReporterConfig
    : public TArchiveReporterConfig
{
public:
    TArchiveHandlerConfigPtr DistributedQueriesHandler;
    TArchiveHandlerConfigPtr SecondaryQueriesHandler;
    TArchiveHandlerConfigPtr AncestorQueryIdsHandler;

    TString User;

    TQueryStatisticsReporterConfig();
};

DEFINE_REFCOUNTED_TYPE(TQueryStatisticsReporterConfig);

////////////////////////////////////////////////////////////////////////////////

class TGossipConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Period to run the gossip procedure.
    //! Note: TPeriodicExecutor counts down the period since the completion of previous invocation,
    //! so the actual period will be in [Period, Period + Timeout].
    TDuration Period;
    //! Timeout for the gossip request. If it is exceeded, the instance is assumed to be dead.
    TDuration Timeout;

    //! We will ignore ping from unknown instances if discovery is younger than this.
    TDuration UnknownInstanceAgeThreshold;
    //! How many times we will handle ping from an unknown instance before ignoring it.
    int UnknownInstancePingLimit;
    //! Try to ping banned instances. It can help to prevent ban expiration for dead instances and
    //! to find mistakenly banned instances. 
    bool PingBanned;
    //! Allow to unban the instance after successful gossip request.
    //! It can help to restore discovery list faster if the instance was banned because of
    //! transient error (e.g. temporary network overload).
    bool AllowUnban;

    TGossipConfig();
};

DEFINE_REFCOUNTED_TYPE(TGossipConfig);

////////////////////////////////////////////////////////////////////////////////

class TInvokerLivenessCheckerConfig
    : public NYTree::TYsonSerializable
{
public:
    bool Enabled;
    TDuration Period;
    TDuration Timeout;

    TInvokerLivenessCheckerConfig();
};

DEFINE_REFCOUNTED_TYPE(TInvokerLivenessCheckerConfig);

////////////////////////////////////////////////////////////////////////////////

class TYtConfig
    : public NYTree::TYsonSerializable
{
public:
    //! Clique id = id of containing operation.
    TGuid CliqueId;
    //! Instance id = job id of containing job.
    TGuid InstanceId;
    //! Address override when entering discovery group.
    std::optional<TString> Address;

    TSlruCacheConfigPtr ClientCache;

    // COMPAT(max42): deprecate these.
    std::optional<bool> ValidateOperationAccess;
    std::optional<TDuration> OperationAclUpdatePeriod;

    TSecurityManagerConfigPtr SecurityManager;

    //! User for communication with YT.
    TString User;

    NTableClient::TTableWriterConfigPtr TableWriter;

    TMemoryWatchdogConfigPtr MemoryWatchdog;

    //! Note that CliqueId will be added to Directory automatically.
    TDiscoveryConfigPtr Discovery;

    TGossipConfigPtr Gossip;

    TInvokerLivenessCheckerConfigPtr ControlInvokerChecker;

    //! Config for cache which is used for checking read permissions to tables.
    NSecurityClient::TPermissionCacheConfigPtr PermissionCache;

    //! Config for cache which is used for getting table's attributes, like id, schema, external_cell_tag, etc.
    NObjectClient::TObjectAttributeCacheConfigPtr TableAttributeCache;

    //! Config for cache which is used for WHERE to PREWHERE optimizator.
    NTableClient::TTableColumnarStatisticsCacheConfigPtr TableColumnarStatisticsCache;

    TDuration ProcessListSnapshotUpdatePeriod;

    int WorkerThreadCount;
    int FetcherThreadCount;

    std::optional<int> CpuLimit;

    //! Subquery logic configuration.
    TSubqueryConfigPtr Subquery;

    NYTree::INodePtr CreateTableDefaultAttributes;

    //! Total amount of memory available for chunk readers.
    i64 TotalReaderMemoryLimit;

    //! Initial memory reservation for reader.
    i64 ReaderMemoryRequirement;

    THealthCheckerConfigPtr HealthChecker;

    TShowTablesConfigPtr ShowTables;

    bool EnableDynamicTables;

    TDuration TotalMemoryTrackerUpdatePeriod;

    TQuerySettingsPtr QuerySettings;

    NTableClient::TTableReaderConfigPtr TableReader;

    TQueryStatisticsReporterConfigPtr QueryStatisticsReporter;

    TYtConfig();
};

DEFINE_REFCOUNTED_TYPE(TYtConfig);

////////////////////////////////////////////////////////////////////////////////

class TLauncherConfig
    : public NYTree::TYsonSerializable
{
public:
    int Version;

    TLauncherConfig();
};

DEFINE_REFCOUNTED_TYPE(TLauncherConfig);

////////////////////////////////////////////////////////////////////////////////
//
// Values in braces are not defined explicitly, but rather taken in account when setting
// the rest of the values. Values starting with hash sign are defined explicitly.
//
// | <================================================= #MemoryLimit ==========================================> |
// | <================= #MaxServerMemoryUsage =================> | <========== (ClickHouseWatermark) ==========> |
// | #Reader | #UncompressedBlockCache | (CH Memory + Footprint) |                       | #WatchdogOomWatermark |
// |                                                | <============== #WatchdogOomWindowWatermark =============> |
//
//                                                         ^              ^                     ^                  ^
// If min rss over 15 min window resides in this __________|              |                     |                  |
// range, instance performs graceful self-interruption.                   |                     |                  |
//                                                                        |                     |                  |
// If rss goes here, CH does not allow any new memory allocation. ________|                     |                  |
//                                                                                              |                  |
// If rss goes here, instance performs harakiri. _______________________________________________|                  |
//                                                                                                                 |
// If rss goes here, YT kills the instance. _______________________________________________________________________|
//
// Memory tracking is one hell of a job.

class TMemoryConfig
    : public NYTree::TYsonSerializable
{
public:
    std::optional<i64> Reader;
    std::optional<i64> UncompressedBlockCache;
    std::optional<i64> ChunkMetaCache;
    std::optional<i64> MemoryLimit;
    std::optional<i64> MaxServerMemoryUsage;
    std::optional<i64> WatchdogOomWatermark;
    std::optional<i64> WatchdogOomWindowWatermark;

    TMemoryConfig();
};

DEFINE_REFCOUNTED_TYPE(TMemoryConfig);

////////////////////////////////////////////////////////////////////////////////

struct TPorts
{
    // YT ports.
    int Monitoring = 0;
    int Rpc = 0;
    // CH ports.
    int Http = 0;
    int Tcp = 0;
};

class TClickHouseServerBootstrapConfig
    : public TServerConfig
{
public:
    NApi::NNative::TConnectionConfigPtr ClusterConnection;

    TClickHouseConfigPtr ClickHouse;

    TYtConfigPtr Yt;

    //! Instance will not shutdown during this period of time after receiving signal even
    //! if there are not any running queries.
    //! To avoid receiving queries after shutdown, this value should be greater than gossip period.
    TDuration GracefulInterruptionDelay;

    //! Hard timeout for process termination after receiving the interruption signal.
    //! If the timeout is exceeded, the process will be forcefully terminated and the job will be marked as failed.
    TDuration InterruptionTimeout;

    TLauncherConfigPtr Launcher;

    // COMPAT(max42): deprecate in favor of yt/cpu_limit.
    std::optional<int> CpuLimit;

    TMemoryConfigPtr Memory;

    TPorts GetPorts() const;

    TClickHouseServerBootstrapConfig();
};

DEFINE_REFCOUNTED_TYPE(TClickHouseServerBootstrapConfig);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NClickHouseServer
