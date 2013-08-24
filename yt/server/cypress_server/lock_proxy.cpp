#include "stdafx.h"
#include "lock_proxy.h"
#include "lock.h"
#include "private.h"

#include <ytlib/ytree/fluent.h>

#include <ytlib/cypress_client/lock_ypath.pb.h>

#include <server/object_server/object_detail.h>

#include <server/transaction_server/transaction.h>

namespace NYT {
namespace NCypressServer {

using namespace NYson;
using namespace NYTree;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TLockProxy
    : public NObjectServer::TNonversionedObjectProxyBase<TLock>
{
public:
    TLockProxy(NCellMaster::TBootstrap* bootstrap, TLock* lock)
        : TBase(bootstrap, lock)
    {
        Logger = CypressServerLogger;
    }

private:
    typedef TNonversionedObjectProxyBase<TLock> TBase;

    virtual void ListSystemAttributes(std::vector<TAttributeInfo>* attributes) override
    {
        const auto* lock = GetThisTypedImpl();
        attributes->push_back("state");
        attributes->push_back("transaction_id");
        attributes->push_back("mode");
        attributes->push_back(TAttributeInfo("child_key", lock->Request().ChildKey));
        attributes->push_back(TAttributeInfo("attribute_key", lock->Request().AttributeKey));
        TBase::ListSystemAttributes(attributes);
    }

    virtual bool GetSystemAttribute(const Stroka& key, IYsonConsumer* consumer) override
    {
        const auto* lock = GetThisTypedImpl();

        if (key == "state") {
            BuildYsonFluently(consumer)
                .Value(lock->GetState());
            return true;
        }

        if (key == "transaction_id") {
            BuildYsonFluently(consumer)
                .Value(lock->GetTransaction()->GetId());
            return true;
        }

        if (key == "mode") {
            BuildYsonFluently(consumer)
                .Value(lock->Request().Mode);
            return true;
        }

        if (key == "child_key" && lock->Request().ChildKey) {
            BuildYsonFluently(consumer)
                .Value(*lock->Request().ChildKey);
            return true;
        }

        if (key == "attribute_key" && lock->Request().AttributeKey) {
            BuildYsonFluently(consumer)
                .Value(*lock->Request().AttributeKey);
            return true;
        }

        return TBase::GetSystemAttribute(key, consumer);
    }

};

IObjectProxyPtr CreateLockProxy(
    NCellMaster::TBootstrap* bootstrap,
    TLock* lock)
{
    return New<TLockProxy>(bootstrap, lock);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NCypressServer
} // namespace NYT

