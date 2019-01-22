#pragma once

#include <mapreduce/yt/interface/common.h>
#include <mapreduce/yt/raw_client/raw_requests.h>

#include <mapreduce/yt/http/requests.h>

#include <util/str_stl.h>
#include <util/system/mutex.h>
#include <util/generic/hash.h>

namespace NYT {
namespace NDetail {

////////////////////////////////////////////////////////////////////////////////

class IAbortable
    : public TThrRefBase
{
public:
    virtual void Abort() = 0;
    virtual TString GetType() const = 0;
};

using IAbortablePtr = TIntrusivePtr<IAbortable>;

////////////////////////////////////////////////////////////////////////////////

class TTransactionAbortable
    : public IAbortable
{
public:
    TTransactionAbortable(const TAuth& auth, const TTransactionId& transactionId);
    void Abort() override;
    TString GetType() const override;

private:
    TAuth Auth_;
    TTransactionId TransactionId_;
};

////////////////////////////////////////////////////////////////////////////////

class TOperationAbortable
    : public IAbortable
{
public:
    TOperationAbortable(const TAuth& auth, const TOperationId& operationId);
    void Abort() override;
    TString GetType() const override;

private:
    TAuth Auth_;
    TOperationId OperationId_;
};

////////////////////////////////////////////////////////////////////////////////

class TAbortableRegistry
    : public TThrRefBase
{
public:
    TAbortableRegistry() = default;
    static ::TIntrusivePtr<TAbortableRegistry> Get();

    void AbortAllAndBlockForever();
    void Add(const TGUID& id, IAbortablePtr abortable);
    void Remove(const TGUID& id);

private:
    THashMap<TGUID, IAbortablePtr> ActiveAbortables_;
    TMutex Lock_;
    bool Running_ = true;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NDetail
} // namespace NYT
