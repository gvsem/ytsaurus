#include "stdafx.h"
#include "rich.h"

#include <ytlib/misc/error.h>
#include <ytlib/yson/yson_consumer.h>
#include <ytlib/ytree/fluent.h>

namespace NYT {
namespace NYPath {

using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TRichYPath::TRichYPath()
{ }

TRichYPath::TRichYPath(const TRichYPath& other)
    : Path_(other.Path_)
    , Attributes_(~other.Attributes_ ? other.Attributes_->Clone() : NULL)
{ }

TRichYPath::TRichYPath(const char* path)
    : Path_(path)
{ }

TRichYPath::TRichYPath(const TYPath& path)
    : Path_(path)
{ }

TRichYPath::TRichYPath(TRichYPath&& other)
    : Path_(MoveRV(other.Path_))
    , Attributes_(other.Attributes_)
{ }

TRichYPath::TRichYPath(const TYPath& path, const IAttributeDictionary& attributes)
    : Path_(path)
    , Attributes_(attributes.Clone())
{ }

const TYPath& TRichYPath::GetPath() const
{
    return Path_;
}

void TRichYPath::SetPath(const TYPath& path)
{
    Path_ = path;
}

const IAttributeDictionary& TRichYPath::Attributes() const
{
    return ~Attributes_ ? *Attributes_ : EmptyAttributes();
}

IAttributeDictionary& TRichYPath::Attributes()
{
    if (!Attributes_) {
        Attributes_ = CreateEphemeralAttributes();
    }
    return *Attributes_;
}

TRichYPath& TRichYPath::operator = (const TRichYPath& other)
{
    if (this != &other) {
        Path_ = other.Path_;
        Attributes_ = ~other.Attributes_ ? other.Attributes_->Clone() : NULL;
    }
    return *this;
}

////////////////////////////////////////////////////////////////////////////////

Stroka ToString(const TRichYPath& path)
{
    auto keys = path.Attributes().List();
    if (keys.empty()) {
        return path.GetPath();
    }

    return
        Stroka('<') +
        ConvertToYsonString(path.Attributes()).Data() +
        Stroka('>') +
        path.GetPath();
}

void Serialize(const TRichYPath& richPath, NYson::IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginAttributes()
            .Items(richPath.Attributes())
        .EndAttributes()
        .Scalar(richPath.GetPath());
}

void Deserialize(TRichYPath& richPath, INodePtr node)
{
    if (node->GetType() != ENodeType::String) {
        THROW_ERROR_EXCEPTION("YPath can only be parsed from String");
    }
    richPath.SetPath(node->GetValue<Stroka>());
    richPath.Attributes().Clear();
    richPath.Attributes().MergeFrom(node->Attributes());
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYPath
} // namespace NYT
