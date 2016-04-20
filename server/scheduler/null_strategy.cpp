#include "null_strategy.h"
#include "exec_node.h"
#include "job.h"
#include "operation.h"
#include "operation_controller.h"
#include "scheduler_strategy.h"

#include <yt/core/misc/common.h>

namespace NYT {
namespace NScheduler {

using namespace NYson;

////////////////////////////////////////////////////////////////////

class TNullStrategy
    : public ISchedulerStrategy
{
public:
    virtual void ScheduleJobs(ISchedulingContext* /*context*/) override
    { }

    virtual TError CanAddOperation(TOperationPtr operation) override
    {
        return TError();
    }

    virtual void BuildOperationAttributes(const TOperationId& /*operationId*/, IYsonConsumer* /*consumer*/) override
    { }

    virtual void BuildOperationProgress(const TOperationId& /*operationId*/, IYsonConsumer* /*consumer*/) override
    { }

    virtual void BuildBriefOperationProgress(const TOperationId& /*operationId*/, IYsonConsumer* /*consumer*/) override
    { }

    virtual Stroka GetOperationLoggingProgress(const TOperationId& /*operationId*/) override
    {
        return "";
    }

    virtual void BuildOrchid(IYsonConsumer* /*consumer*/) override
    { }

    virtual void BuildBriefSpec(const TOperationId& /*operationId*/, IYsonConsumer* /*consumer*/) override
    { }

};

std::unique_ptr<ISchedulerStrategy> CreateNullStrategy(ISchedulerStrategyHost* host)
{
    UNUSED(host);
    return std::unique_ptr<ISchedulerStrategy>(new TNullStrategy());
}

////////////////////////////////////////////////////////////////////

} // namespace NScheduler
} // namespace NYT

