#include "stdafx.h"
#include "private.h"
#include "cgroup.h"

#include <core/misc/fs.h>
#include <core/misc/error.h>

#include <util/folder/dirut.h>
#include <util/system/fs.h>
#include <util/string/split.h>

#include <fstream>
#include <sstream>

namespace NYT {
namespace NCGroup {

////////////////////////////////////////////////////////////////////////////////

static auto& Logger = CGroupLogger;
static const char* CGroupRootPath = "/sys/fs/cgroup";

////////////////////////////////////////////////////////////////////////////////

namespace {

Stroka GetParentFor(const Stroka& type)
{
    auto rawData = TFileInput("/proc/self/cgroup").ReadAll();
    auto result = ParseCurrentProcessCGroups(TStringBuf(rawData.data(), rawData.size()));
    return result[type];
}


yvector<Stroka> ReadAllValues(const Stroka& filename)
{
    auto raw = TFileInput(filename).ReadAll();
    yvector<Stroka> values;
    Split(raw.data(), " \n", values);
    return values;
}

}

////////////////////////////////////////////////////////////////////////////////

TCGroup::TCGroup(const Stroka& type, const Stroka& name)
    : FullPath_(NFS::CombinePaths(NFS::CombinePaths(NFS::CombinePaths(CGroupRootPath,  type), GetParentFor(type)), name))
    , Created_(false)
{ }

TCGroup::~TCGroup()
{
    if (Created_) {
        try {
            Destroy();
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Unable to destroy cgroup %s", ~FullPath_.Quote());
        }
    }
}

void TCGroup::Create()
{
    LOG_INFO("Create cgroup %s", ~FullPath_.Quote());

#ifdef _linux_
    int hasError = Mkdir(FullPath_.data(), 0755);
    if (hasError != 0) {
        THROW_ERROR_EXCEPTION("Unable to create cgroup %s", ~FullPath_.Quote())
            << TError::FromSystem();
    }
    Created_ = true;
#endif
}

void TCGroup::Destroy()
{
    LOG_INFO("Destroy cgroup %s", ~FullPath_.Quote());

#ifdef _linux_
    YCHECK(Created_);

    int hasError = NFs::Remove(FullPath_.data());
    if (hasError != 0) {
        THROW_ERROR(TError::FromSystem());
    }
    Created_ = false;
#endif
}

void TCGroup::AddCurrentProcess()
{
#ifdef _linux_
    auto pid = getpid();
    LOG_INFO("Add process %d to cgroup %s", pid, ~FullPath_.Quote());

    std::ofstream tasks(NFS::CombinePaths(FullPath_, "tasks").data(), std::ios_base::app);
    tasks << getpid() << std::endl;
#endif
}

std::vector<int> TCGroup::GetTasks() const
{
    std::vector<int> results;
#ifdef _linux_
    auto values = ReadAllValues(NFS::CombinePaths(FullPath_, "tasks"));
    for (const auto& value : values) {
        int pid = FromString<int>(value);
        results.push_back(pid);
    }
#endif
    return results;
}

const Stroka& TCGroup::GetFullPath() const
{
    return FullPath_;
}

bool TCGroup::IsCreated() const
{
    return Created_;
}

////////////////////////////////////////////////////////////////////////////////

#ifdef _linux_

TDuration FromJiffies(i64 jiffies)
{
    long ticksPerSecond = sysconf(_SC_CLK_TCK);
    return TDuration::MicroSeconds(1000 * 1000 * jiffies/ ticksPerSecond);
}

#endif

////////////////////////////////////////////////////////////////////////////////

TCpuAccounting::TStats::TStats()
    : User(0)
    , System(0)
{ }

TCpuAccounting::TCpuAccounting(const Stroka& name)
    : TCGroup("cpuacct", name)
{ }

TCpuAccounting::TStats TCpuAccounting::GetStats()
{
    TCpuAccounting::TStats result;
#ifdef _linux_
    const auto filename = NFS::CombinePaths(GetFullPath(), "cpuacct.stat");
    auto values = ReadAllValues(filename);
    if (values.size() != 4) {
        THROW_ERROR_EXCEPTION("Unable to parse %s: expected 4 values, got %d", ~filename.Quote(), values.size());
    }

    Stroka type[2];
    i64 jiffies[2];

    for (int i = 0; i < 2; ++i) {
        type[i] = values[2 * i];
        jiffies[i] = FromString<i64>(values[2 * i + 1]);
    }

    for (int i = 0; i < 2; ++ i) {
        if (type[i] == "user") {
            result.User = FromJiffies(jiffies[i]);
        } else if (type[i] == "system") {
            result.System = FromJiffies(jiffies[i]);
        }
    }
#endif
    return result;
}

////////////////////////////////////////////////////////////////////////////////

TBlockIO::TStats::TStats()
    : TotalSectors(0)
    , BytesRead(0)
    , BytesWritten(0)
{ }

TBlockIO::TBlockIO(const Stroka& name)
    : TCGroup("blkio", name)
{ }

TBlockIO::TStats TBlockIO::GetStats()
{
    TBlockIO::TStats result;
#ifdef _linux_
    {
        const auto filename = NFS::CombinePaths(GetFullPath(), "blkio.io_service_bytes");
        auto values = ReadAllValues(filename);

        result.BytesRead = result.BytesWritten = 0;
        int lineNumber = 0;
        while (3 * lineNumber + 2 < values.size()) {
            const Stroka& deviceId = values[3 * lineNumber];
            const Stroka& type = values[3 * lineNumber + 1];
            i64 bytes = FromString<i64>(values[3 * lineNumber + 2]);

            if (deviceId.Size() <= 2 || deviceId.has_prefix("8:")) {
                THROW_ERROR_EXCEPTION("Unable to parse %s: %s should start from 8:", ~filename.Quote(), ~deviceId);
            }

            if (type == "Read") {
                result.BytesRead += bytes;
            } else if (type == "Write") {
                result.BytesWritten += bytes;
            } else {
                if (type != "Sync" && type != "Async" && type != "Total") {
                    THROW_ERROR_EXCEPTION("Unable to parse %s: unexpected stat type %s", ~filename.Quote(), ~type);
                }
            }
            ++lineNumber;
        }
    }
    {
        const auto filename = NFS::CombinePaths(GetFullPath(), "blkio.sectors");
        auto values = ReadAllValues(filename);

        result.TotalSectors = 0;
        int lineNumber = 0;
        while (2 * lineNumber < values.size()) {
            const Stroka& deviceId = values[2 * lineNumber];
            i64 sectors = FromString<i64>(values[2 * lineNumber + 1]);

            if (deviceId.Size() <= 2 || deviceId.has_prefix("8:")) {
                THROW_ERROR_EXCEPTION("Unable to parse %s: %s should start from 8:", ~filename.Quote(), ~deviceId);
            }

            result.TotalSectors += sectors;
            ++lineNumber;
        }
    }
#endif
    return result;
}

////////////////////////////////////////////////////////////////////////////////

std::map<Stroka, Stroka> ParseCurrentProcessCGroups(TStringBuf str)
{
    std::map<Stroka, Stroka> result;

    yvector<Stroka> values;
    Split(str.data(), ":\n", values);
    for (size_t i = 0; i + 2 < values.size(); i += 3) {
        FromString<int>(values[i]);

        const Stroka& subsystemsSet = values[i + 1];
        const Stroka& name = values[i + 2];

        yvector<Stroka> subsystems;
        Split(subsystemsSet.data(), ",", subsystems);
        for (const auto& subsystem : subsystems) {
            if (!subsystem.has_prefix("name=")) {
                int start = 0;
                if (name.has_prefix("/")) {
                    start = 1;
                }
                result[subsystem] = name.substr(start);
            }
        }
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCGroup
} // namespace NYT
