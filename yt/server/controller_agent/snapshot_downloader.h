#pragma once

#include "private.h"

#include <yt/server/scheduler/public.h>

#include <yt/core/logging/log.h>

#include <yt/core/misc/ref.h>

namespace NYT {
namespace NControllerAgent {

////////////////////////////////////////////////////////////////////////////////

class TSnapshotDownloader
    : public TRefCounted
{
public:
    TSnapshotDownloader(
        TControllerAgentConfigPtr config,
        NScheduler::TBootstrap* bootstrap,
        const TOperationId& operationId);

    TSharedRef Run(const NYTree::TYPath& snapshotPath);

private:
    const TControllerAgentConfigPtr Config_;
    NScheduler::TBootstrap* const Bootstrap_;
    const TOperationId OperationId_;

    const NLogging::TLogger Logger;

};

DEFINE_REFCOUNTED_TYPE(TSnapshotDownloader)

////////////////////////////////////////////////////////////////////////////////

} // namespace NControllerAgent
} // namespace NYT
