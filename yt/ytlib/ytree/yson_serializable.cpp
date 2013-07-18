#include "stdafx.h"
#include "yson_serializable.h"

#include <ytlib/ytree/node.h>
#include <ytlib/ytree/ephemeral_node_factory.h>
#include <ytlib/ytree/ypath_detail.h>

#include <ytlib/yson/consumer.h>

namespace NYT {

using namespace NYTree;
using namespace NYPath;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

TYsonSerializableLite::TYsonSerializableLite()
    : KeepOptions_(false)
{ }

IMapNodePtr TYsonSerializableLite::GetOptions() const
{
    YCHECK(KeepOptions_);
    return Options;
}

std::vector<Stroka> TYsonSerializableLite::GetRegisteredKeys() const
{
    std::vector<Stroka> result;
    FOREACH (const auto& pair, Parameters) {
        result.push_back(pair.first);
    }
    return result;
}

void TYsonSerializableLite::Load(
    INodePtr node,
    bool validate,
    bool setDefaults,
    const TYPath& path)
{
    YCHECK(node);

    if (setDefaults) {
        SetDefaults();
    }

    auto mapNode = node->AsMap();
    FOREACH (const auto& pair, Parameters) {
        auto name = pair.first;
        auto childPath = path + "/" + name;
        auto child = mapNode->FindChild(name); // can be NULL
        pair.second->Load(child, childPath);
    }

    if (KeepOptions_) {
        Options = GetEphemeralNodeFactory()->CreateMap();
        FOREACH (const auto& pair, mapNode->GetChildren()) {
            const auto& key = pair.first;
            auto child = pair.second;
            if (Parameters.find(key) == Parameters.end()) {
                Options->AddChild(ConvertToNode(child), key);
            }
        }
    }

    if (validate) {
        Validate(path);
    }

    OnLoaded();
}

void TYsonSerializableLite::Validate(const TYPath& path) const
{
    FOREACH (auto pair, Parameters) {
        pair.second->Validate(path + "/" + pair.first);
    }

    try {
        FOREACH (const auto& validator, Validators) {
            validator.Run();
        }
    } catch (const std::exception& ex) {
        THROW_ERROR_EXCEPTION("Validation failed at %s", ~path)
            << ex;
    }
}

void TYsonSerializableLite::SetDefaults()
{
    FOREACH (auto pair, Parameters) {
        pair.second->SetDefaults();
    }
    FOREACH (const auto& initializer, Initializers) {
        initializer.Run();
    }
}

void TYsonSerializableLite::OnLoaded()
{ }

void TYsonSerializableLite::Save(
    IYsonConsumer* consumer,
    bool sortKeys) const
{
    std::vector<std::pair<Stroka, NConfig::IParameterPtr>> parameters;
    FOREACH (const auto& pair, Parameters) {
        parameters.push_back(pair);
    }

    if (sortKeys) {
        typedef std::pair<Stroka, NConfig::IParameterPtr> TPair;
        std::sort(
            parameters.begin(),
            parameters.end(),
            [] (const TPair& lhs, const TPair& rhs) {
                return lhs.first < rhs.first;
            });
    }

    consumer->OnBeginMap();
    FOREACH (const auto& pair, parameters) {
        const auto& key = pair.first;
        const auto& parameter = pair.second;
        if (parameter->IsPresent()) {
            consumer->OnKeyedItem(key);
            parameter->Save(consumer);
        }
    }
    consumer->OnEndMap();
}

////////////////////////////////////////////////////////////////////////////////

void TBinaryYsonSerializer::Save(TStreamSaveContext& context, const TYsonSerializableLite& obj)
{
    auto str = ConvertToYsonStringStable(obj);
    NYT::Save(context, str);
}

void TBinaryYsonSerializer::Load(TStreamLoadContext& context, TYsonSerializableLite& obj)
{
    auto str = NYT::Load<TYsonString>(context);
    auto node = ConvertTo<INodePtr>(str);
    obj.Load(node);
}

////////////////////////////////////////////////////////////////////////////////

void Serialize(const TYsonSerializableLite& value, IYsonConsumer* consumer)
{
    value.Save(consumer);
}

void Deserialize(TYsonSerializableLite& value, INodePtr node)
{
    value.Load(node);
}

TYsonString ConvertToYsonStringStable(const TYsonSerializableLite& value)
{
    Stroka result;
    TStringOutput output(result);
    TYsonWriter writer(&output, EYsonFormat::Binary, EYsonType::Node);
    value.Save(
        &writer,
        true); // truth matters :)
    return TYsonString(result, EYsonType::Node);   
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
