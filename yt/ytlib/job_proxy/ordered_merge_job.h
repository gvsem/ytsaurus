﻿#pragma once

#include "public.h"
#include "job.h"

#include <ytlib/election/leader_lookup.h>
#include <ytlib/table_client/public.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

class TOrderedMergeJob
    : public IJob
{
public:
    TOrderedMergeJob(
        const TJobIOConfigPtr& config,
        const NElection::TLeaderLookup::TConfigPtr& masterConfig,
        const NScheduler::NProto::TMergeJobSpec& mergeJobSpec);

    NScheduler::NProto::TJobResult Run();

private:
    NTableClient::ISyncReaderPtr Reader;
    NTableClient::ISyncWriterPtr Writer;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYTNYT