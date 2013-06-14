#pragma once

#include "public.h"

#include <ytlib/misc/serialize.h>

#include <ytlib/yson/public.h>

namespace NYT {
namespace NScheduler {

////////////////////////////////////////////////////////////////////////////////

//! Describes a cumulative statistics for jobs of a particular type.
struct TTotalJobStatistics
{
    TTotalJobStatistics();

    //! Total time spent by jobs.
    TDuration Time;

    void Persist(TStreamPersistenceContext& context);

};

void Serialize(const TTotalJobStatistics& statistics, NYson::IYsonConsumer* consumer);

////////////////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT
