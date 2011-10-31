#include "tree_visitor.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

TTreeVisitor::TTreeVisitor(IYsonConsumer* consumer, bool visitAttributes)
    : Consumer(consumer)
    , VisitAttributes_(visitAttributes)
{ }

void TTreeVisitor::Visit(INode::TPtr root)
{
    VisitAny(root);
}

void TTreeVisitor::VisitAny(INode::TPtr node)
{
    auto attributes = node->GetAttributes();
    bool hasAttributes = ~attributes != NULL && VisitAttributes_;

    switch (node->GetType()) {
        case ENodeType::String:
        case ENodeType::Int64:
        case ENodeType::Double:
            VisitScalar(node, hasAttributes);
            break;

        case ENodeType::Entity:
            VisitEntity(node, hasAttributes);
            break;

        case ENodeType::List:
            VisitList(node->AsList(), hasAttributes);
            break;

        case ENodeType::Map:
            VisitMap(node->AsMap(), hasAttributes);
            break;

        default:
            YUNREACHABLE();
    }

    if (hasAttributes) {
        VisitAttributes(node->GetAttributes());
    }
}

void TTreeVisitor::VisitScalar(INode::TPtr node, bool hasAttributes)
{
    switch (node->GetType()) {
        case ENodeType::String:
            Consumer->OnStringScalar(node->GetValue<Stroka>(), hasAttributes);
            break;

        case ENodeType::Int64:
            Consumer->OnInt64Scalar(node->GetValue<i64>(), hasAttributes);
            break;

        case ENodeType::Double:
            Consumer->OnDoubleScalar(node->GetValue<double>(), hasAttributes);
            break;

        default:
            YUNREACHABLE();
    }
}

void TTreeVisitor::VisitEntity(INode::TPtr node, bool hasAttributes)
{
    UNUSED(node);
    Consumer->OnEntity(hasAttributes);
}

void TTreeVisitor::VisitList(IListNode::TPtr node, bool hasAttributes)
{
    Consumer->OnBeginList();
    for (int i = 0; i < node->GetChildCount(); ++i) {
        auto child = node->GetChild(i);
        Consumer->OnListItem();
        VisitAny(child);
    }
    Consumer->OnEndList(hasAttributes);
}

void TTreeVisitor::VisitMap(IMapNode::TPtr node, bool hasAttributes)
{
    Consumer->OnBeginMap();
    FOREACH(const auto& pair, node->GetChildren()) {
        Consumer->OnMapItem(pair.First());
        VisitAny(pair.Second());
    }
    Consumer->OnEndMap(hasAttributes);
}

void TTreeVisitor::VisitAttributes(IMapNode::TPtr node)
{
    Consumer->OnBeginAttributes();
    FOREACH(const auto& pair, node->GetChildren()) {
        Consumer->OnAttributesItem(pair.First());
        VisitAny(pair.Second());
    }
    Consumer->OnEndAttributes();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
