#include "stdafx.h"
#include "attribute_set.h"

#include <ytlib/misc/serialize.h>

#include <server/cell_master/serialization_context.h>

namespace NYT {
namespace NObjectServer {

////////////////////////////////////////////////////////////////////////////////

TAttributeSet::TAttributeSet()
{ }

TAttributeSet::TAttributeSet(const TVersionedObjectId&)
{ }

void TAttributeSet::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, Attributes_);
}

void TAttributeSet::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    Load(context, Attributes_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NObjectServer
} // namespace NYT
