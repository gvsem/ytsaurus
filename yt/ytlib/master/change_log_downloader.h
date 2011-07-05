#pragma once

#include "common.h"
#include "master_state.h"
#include "decorated_master_state.h"
#include "master_state_manager_rpc.h"
#include "cell_manager.h"
#include "change_log_writer.h"

#include "../rpc/client.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TChangeLogDownloader
    : private TNonCopyable
{
public:
    struct TConfig
    {
        TDuration LookupTimeout;
        TDuration ReadTimeout;
        i32 RecordsPerRequest;

        TConfig()
            : LookupTimeout(TDuration::Seconds(2))
            , ReadTimeout(TDuration::Seconds(2))
            , RecordsPerRequest(1024)
        { }
    };

    enum EResult
    {
        OK,
        ChangeLogNotFound,
        ChangeLogUnavailable,
        RemoteError
    };

    TChangeLogDownloader(const TConfig& config, TCellManager::TPtr cellManager);

    EResult Download(TMasterStateId stateId, TAsyncChangeLog& asyncChangeLog);

private:
    typedef TMasterStateManagerProxy TProxy;

    TConfig Config;
    TCellManager::TPtr CellManager;

    TMasterId GetChangeLogSource(TMasterStateId stateId);

    EResult DownloadChangeLog(
        TMasterStateId stateId,
        TMasterId sourceId,
        TAsyncChangeLog& asyncChangeLog);

    static void OnResponse(
        TProxy::TRspGetChangeLogInfo::TPtr response,
        TParallelAwaiter::TPtr awaiter,
        TAsyncResult<TMasterId>::TPtr asyncResult,
        TMasterId masterId,
        TMasterStateId stateId);
    static void OnComplete(
        TAsyncResult<TMasterId>::TPtr asyncResult);

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
