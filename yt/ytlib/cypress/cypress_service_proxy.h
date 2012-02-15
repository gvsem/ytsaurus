#pragma once

#include "id.h"
#include "cypress_service.pb.h"

#include <ytlib/ytree/ypath_client.h>

namespace NYT {
namespace NCypress {

////////////////////////////////////////////////////////////////////////////////

extern const NYTree::TYPath ObjectIdMarker;
extern const NYTree::TYPath TransactionIdMarker;

//! Creates the YPath pointing to an object with a given id.
NYTree::TYPath FromObjectId(const TObjectId& id);

//! Prepends a given YPath with transaction id marker.
NYTree::TYPath WithTransaction(const NYTree::TYPath& path, const TTransactionId& id);

////////////////////////////////////////////////////////////////////////////////

class TCypressServiceProxy
    : public NRpc::TProxyBase
{
public:
    typedef TIntrusivePtr<TCypressServiceProxy> TPtr;

    static Stroka GetServiceName()
    {
        return "CypressService";
    }

    TCypressServiceProxy(NRpc::IChannel* channel)
        : TProxyBase(channel, GetServiceName())
    { }

    DEFINE_RPC_PROXY_METHOD(NProto, Execute);

    //! Executes a single Cypress request.
    template <class TTypedRequest>
    TIntrusivePtr< TFuture< TIntrusivePtr<typename TTypedRequest::TTypedResponse> > >
    Execute(TTypedRequest* innerRequest);

    class TReqExecuteBatch;
    class TRspExecuteBatch;

    //! A batched request to Cypress that holds a vector of individual requests that
    //! are transferred within a single RPC envelope.
    class TReqExecuteBatch
        : public NRpc::TClientRequest
    {
    public:
        typedef TIntrusivePtr<TReqExecuteBatch> TPtr;

        TReqExecuteBatch(
            NRpc::IChannel* channel,
            const Stroka& path,
            const Stroka& verb);

        TFuture< TIntrusivePtr<TRspExecuteBatch> >::TPtr Invoke();

        // Override base method for fluent use.
        TIntrusivePtr<TReqExecuteBatch> SetTimeout(TNullable<TDuration> timeout)
        {
            TClientRequest::SetTimeout(timeout);
            return this;
        }

        //! Add an individual request into the batch.
        TIntrusivePtr<TReqExecuteBatch> AddRequest(NYTree::TYPathRequest* innerRequest);

        //! Returns the current number of individual requests in the batch.
        int GetSize() const;

    private:
        NProto::TRspExecute Body;

        virtual TBlob SerializeBody() const;

    };

    //! A response to a batched request.
    /*!
     *  This class holds a vector of messages representing responses to individual
     *  requests that were earlier sent to Cypress.
     *  
     *  The length of this vector (see #Size) coincides to that of the requests vector.
     *  
     *  Individual responses can be extracted by calling #Get. Since they may be of
     *  different actual types, the client must supply an additional type parameter.
     *  
     */
    class TRspExecuteBatch
        : public NRpc::TClientResponse
    {
    public:
        typedef TIntrusivePtr<TRspExecuteBatch> TPtr;

        TRspExecuteBatch(const NRpc::TRequestId& requestId);

        TFuture<TPtr>::TPtr GetAsyncResult();

        //! Returns the current number of individual responses in the batch.
        int GetSize() const;

        //! Returns the individual response with a given index.
        template <class TTypedResponse>
        TIntrusivePtr<TTypedResponse> GetResponse(int index) const;

        //! Returns the individual generic response with a given index.
        NYTree::TYPathResponse::TPtr GetResponse(int index) const;

    private:
        TFuture<TPtr>::TPtr AsyncResult;
        NProto::TRspExecute Body;
        yvector<int> BeginPartIndexes;

        virtual void FireCompleted();
        virtual void DeserializeBody(const TRef& data);

    };

    //! Mimics the type introduced by DEFINE_RPC_PROXY_METHOD.
    typedef TFuture<TRspExecuteBatch::TPtr> TInvExecuteBatch;

    //! Executes a batched Cypress request.
    TReqExecuteBatch::TPtr ExecuteBatch();

};

////////////////////////////////////////////////////////////////////////////////


} // namespace NCypress
} // namespace NYT

#define CYPRESS_SERVICE_PROXY_INL_H_
#include "cypress_service_proxy-inl.h"
#undef CYPRESS_SERVICE_PROXY_INL_H_
