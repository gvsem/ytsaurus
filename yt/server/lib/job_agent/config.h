#pragma once

#include "public.h"

#include <yt/core/ytree/yson_serializable.h>

#include <yt/core/concurrency/config.h>

namespace NYT::NJobAgent {

////////////////////////////////////////////////////////////////////////////////

class TResourceLimitsConfig
    : public NYTree::TYsonSerializable
{
public:
    int UserSlots;
    double Cpu;
    int Gpu;
    int Network;
    i64 UserMemory;
    i64 SystemMemory;
    int ReplicationSlots;
    i64 ReplicationDataSize;
    int RemovalSlots;
    int RepairSlots;
    i64 RepairDataSize;
    int SealSlots;

    TResourceLimitsConfig()
    {
        // These are some very low default limits.
        // Override for production use.
        RegisterParameter("user_slots", UserSlots)
            .GreaterThanOrEqual(0)
            .Default(1);
        RegisterParameter("cpu", Cpu)
            .GreaterThanOrEqual(0)
            .Default(1);
        RegisterParameter("gpu", Gpu)
            .GreaterThanOrEqual(0)
            .Default(0);
        RegisterParameter("network", Network)
            .GreaterThanOrEqual(0)
            .Default(100);
        RegisterParameter("user_memory", UserMemory)
            .Alias("memory")
            .GreaterThanOrEqual(0)
            .Default(std::numeric_limits<i64>::max());
        RegisterParameter("system_memory", SystemMemory)
            .GreaterThanOrEqual(0)
            .Default(std::numeric_limits<i64>::max());
        RegisterParameter("replication_slots", ReplicationSlots)
            .GreaterThanOrEqual(0)
            .Default(16);
        RegisterParameter("replication_data_size", ReplicationDataSize)
            .Default(10_GB)
            .GreaterThanOrEqual(0);
        RegisterParameter("removal_slots", RemovalSlots)
            .GreaterThanOrEqual(0)
            .Default(16);
        RegisterParameter("repair_slots", RepairSlots)
            .GreaterThanOrEqual(0)
            .Default(4);
        RegisterParameter("repair_data_size", RepairDataSize)
            .Default(4_GB)
            .GreaterThanOrEqual(0);
        RegisterParameter("seal_slots", SealSlots)
            .GreaterThanOrEqual(0)
            .Default(16);
    }
};

DEFINE_REFCOUNTED_TYPE(TResourceLimitsConfig)

////////////////////////////////////////////////////////////////////////////////

class TGpuManagerConfig
    : public NYTree::TYsonSerializable
{
public:
    TDuration HealthCheckTimeout;
    TDuration HealthCheckPeriod;

    TGpuManagerConfig()
    {
        RegisterParameter("health_check_timeout", HealthCheckTimeout)
            .Default(TDuration::Minutes(5));
        RegisterParameter("health_check_period", HealthCheckPeriod)
            .Default(TDuration::Seconds(10));
    }
};

DEFINE_REFCOUNTED_TYPE(TGpuManagerConfig)

////////////////////////////////////////////////////////////////////////////////

class TJobControllerConfig
    : public NYTree::TYsonSerializable
{
public:
    TResourceLimitsConfigPtr ResourceLimits;
    NConcurrency::TThroughputThrottlerConfigPtr StatisticsThrottler;
    TDuration WaitingJobsTimeout;
    TDuration GetJobSpecsTimeout;
    TDuration TotalConfirmationPeriod;

    TDuration CpuOverdraftTimeout;
    TDuration MemoryOverdraftTimeout;

    TDuration ResourceAdjustmentPeriod;

    i64 FreeMemoryWatermark;

    double CpuPerTabletSlot;

    //! Port set has higher priority than StartPort ans PortCount if it is specified.
    int StartPort;
    int PortCount;
    std::optional<THashSet<int>> PortSet;

    //! This is a special testing option.
    //! Instead of normal gpu discovery, it forces the node to believe the number of GPUs passed in the config.
    bool TestGpu;

    TGpuManagerConfigPtr GpuManager;

    TJobControllerConfig()
    {
        RegisterParameter("resource_limits", ResourceLimits)
            .DefaultNew();
        RegisterParameter("statistics_throttler", StatisticsThrottler)
            .DefaultNew();

        // Make it greater than interrupt preemption timeout.
        RegisterParameter("waiting_jobs_timeout", WaitingJobsTimeout)
            .Default(TDuration::Seconds(30));

        RegisterParameter("get_job_specs_timeout", GetJobSpecsTimeout)
            .Default(TDuration::Seconds(5));

        RegisterParameter("total_confirmation_period", TotalConfirmationPeriod)
            .Default(TDuration::Minutes(10));

        RegisterParameter("memory_overdraft_timeout", MemoryOverdraftTimeout)
            .Default(TDuration::Minutes(5));

        RegisterParameter("cpu_overdraft_timeout", CpuOverdraftTimeout)
            .Default(TDuration::Minutes(10));

        RegisterParameter("resource_adjustment_period", ResourceAdjustmentPeriod)
            .Default(TDuration::Seconds(5));

        RegisterParameter("cpu_per_tablet_slot", CpuPerTabletSlot)
            .Default(1.0);

        RegisterParameter("start_port", StartPort)
            .Default(20000);

        RegisterParameter("port_count", PortCount)
            .Default(10000);

        RegisterParameter("port_set", PortSet)
            .Default();

        RegisterParameter("test_gpu", TestGpu)
            .Default(false);

        RegisterParameter("gpu_manager", GpuManager)
            .DefaultNew();

        RegisterParameter("free_memory_watermark", FreeMemoryWatermark)
            .Default(0)
            .GreaterThanOrEqual(0);

        RegisterPreprocessor([&] () {
            // 100 kB/sec * 1000 [nodes] = 100 MB/sec that corresponds to
            // approximate incoming bandwidth of 1Gbit/sec of the scheduler.
            StatisticsThrottler->Limit = 100_KB;
        });
    }
};

DEFINE_REFCOUNTED_TYPE(TJobControllerConfig)

////////////////////////////////////////////////////////////////////////////////

class TStatisticsReporterConfig
    : public NYTree::TYsonSerializable
{
public:
    bool Enabled;
    TDuration ReportingPeriod;
    TDuration MinRepeatDelay;
    TDuration MaxRepeatDelay;
    int MaxInProgressJobDataSize;
    int MaxInProgressJobSpecDataSize;
    int MaxInProgressJobStderrDataSize;
    int MaxInProgressJobFailContextDataSize;
    int MaxItemsInBatch;
    TString User;

    TStatisticsReporterConfig()
    {
        RegisterParameter("enabled", Enabled)
            .Default(true);
        RegisterParameter("reporting_period", ReportingPeriod)
            .Default(TDuration::Seconds(5));
        RegisterParameter("min_repeat_delay", MinRepeatDelay)
            .Default(TDuration::Seconds(10));
        RegisterParameter("max_repeat_delay", MaxRepeatDelay)
            .Default(TDuration::Minutes(5));
        RegisterParameter("max_in_progress_job_data_size", MaxInProgressJobDataSize)
            .Default(250_MB);
        RegisterParameter("max_in_progress_job_spec_data_size", MaxInProgressJobSpecDataSize)
            .Default(250_MB);
        RegisterParameter("max_in_progress_job_stderr_data_size", MaxInProgressJobStderrDataSize)
            .Default(250_MB);
        RegisterParameter("max_in_progress_job_fail_context_data_size", MaxInProgressJobFailContextDataSize)
            .Default(250_MB);
        RegisterParameter("max_items_in_batch", MaxItemsInBatch)
            .Default(1000);
        RegisterParameter("user", User)
            .Default(NRpc::RootUserName);
    }
};

DEFINE_REFCOUNTED_TYPE(TStatisticsReporterConfig)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NJobAgent
