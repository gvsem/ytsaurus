#include "tablet_action.h"

#include <yt/yt/server/lib/tablet_balancer/balancing_helpers.h>

#include <library/cpp/yt/misc/variant.h>

namespace NYT::NTabletBalancer {

////////////////////////////////////////////////////////////////////////////////

TTabletAction::TTabletAction(
    TTabletActionId id,
    const TActionDescriptor& descriptor)
    : Id_(id)
{
    Visit(descriptor,
        [&] (const TMoveDescriptor& descriptor) {
            Kind_ = ETabletActionKind::Move;
            TabletIds_.emplace_back(descriptor.TabletId);
            CellIds_.emplace_back(descriptor.TabletCellId);
        },
        [&] (const TReshardDescriptor& descriptor) {
            Kind_ = ETabletActionKind::Reshard;
            TabletIds_ = std::move(descriptor.Tablets);
            TabletCount_ = descriptor.TabletCount;
        });
}

bool TTabletAction::IsFinished() const
{
    return Lost_ ||
        State_ == ETabletActionState::Completed ||
        State_ == ETabletActionState::Failed;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletBalancer
