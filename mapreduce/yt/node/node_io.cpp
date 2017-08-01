#include "node_io.h"

#include "node_builder.h"
#include "node_visitor.h"

#include <library/yson/parser.h>
#include <library/yson/writer.h>
#include <library/yson/json_writer.h>

#include <library/json/json_reader.h>
#include <library/json/json_value.h>

#include <util/stream/input.h>
#include <util/stream/output.h>
#include <util/stream/str.h>

namespace NYT {

static void WalkJsonTree(const NJson::TJsonValue& jsonValue, NJson::TJsonCallbacks* callbacks)
{
    using namespace NJson;
    switch (jsonValue.GetType()) {
        case JSON_NULL:
            callbacks->OnNull();
            return;
        case JSON_BOOLEAN:
            callbacks->OnBoolean(jsonValue.GetBoolean());
            return;
        case JSON_INTEGER:
            callbacks->OnInteger(jsonValue.GetInteger());
            return;
        case JSON_UINTEGER:
            callbacks->OnUInteger(jsonValue.GetUInteger());
            return;
        case JSON_DOUBLE:
            callbacks->OnDouble(jsonValue.GetDouble());
            return;
        case JSON_STRING:
            callbacks->OnString(jsonValue.GetString());
            return;
        case JSON_MAP:
            {
                callbacks->OnOpenMap();
                for (const auto& item : jsonValue.GetMap()) {
                    callbacks->OnMapKey(item.first);
                    WalkJsonTree(item.second, callbacks);
                }
                callbacks->OnCloseMap();
            }
            return;
        case JSON_ARRAY:
            {
                callbacks->OnOpenArray();
                for (const auto& item : jsonValue.GetArray()) {
                    WalkJsonTree(item, callbacks);
                }
                callbacks->OnCloseArray();
            }
            return;
        case JSON_UNDEFINED:
            ythrow yexception() << "cannot consume undefined json value";
            return;
    }
    Y_UNREACHABLE();
}

static TNode CreateEmptyNodeByType(EYsonType type)
{
    TNode result;
    switch (type) {
        case YT_LIST_FRAGMENT:
            result = TNode::CreateList();
            break;
        case YT_MAP_FRAGMENT:
            result = TNode::CreateMap();
            break;
        default:
            break;
    }
    return result;
}

TNode NodeFromYsonString(const TString& input, EYsonType type)
{
    TStringInput stream(input);

    TNode result = CreateEmptyNodeByType(type);

    TNodeBuilder builder(&result);
    TYsonParser parser(&builder, &stream, type);
    parser.Parse();
    return result;
}

TString NodeToYsonString(const TNode& node, EYsonFormat format)
{
    TStringStream stream;
    TYsonWriter writer(&stream, format);
    TNodeVisitor visitor(&writer);
    visitor.Visit(node);
    return stream.Str();
}

TNode NodeFromJsonString(const TString& input, EYsonType type)
{
    TStringInput stream(input);

    TNode result = CreateEmptyNodeByType(type);

    TNodeBuilder builder(&result);
    TYson2JsonCallbacksAdapter callbacks(&builder, /*throwException*/ true);
    NJson::ReadJson(&stream, &callbacks);
    return result;
}

TNode NodeFromJsonValue(const NJson::TJsonValue& input)
{
    TNode result;
    TNodeBuilder builder(&result);
    TYson2JsonCallbacksAdapter callbacks(&builder, /*throwException*/ true);
    WalkJsonTree(input, &callbacks);
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
