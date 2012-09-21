#pragma once

#include "public.h"

#include <ytlib/ytree/public.h>
#include <ytlib/cypress_client/public.h>
#include <ytlib/transaction_client/public.h>
#include <ytlib/transaction_client/transaction_listener.h>
#include <ytlib/rpc/public.h>
#include <ytlib/logging/tagged_logger.h>
#include <ytlib/misc/ref.h>

namespace NYT {
namespace NFileClient {

////////////////////////////////////////////////////////////////////////////////

//! A client-side facade for writing files.
/*!
 *  The client must call #Open and then feed the data in by calling #Write.
 *  Finally it must call #Close.
 */
class TFileWriter
    : public NTransactionClient::TTransactionListener
{
public:
    //! Initializes an instance.
    TFileWriter(
        TFileWriterConfigPtr config,
        NRpc::IChannelPtr masterChannel,
        NTransactionClient::ITransactionPtr transaction,
        NTransactionClient::TTransactionManagerPtr transactionManager,
        const NYTree::TYPath& path);

    //! Destroys an instance.
    ~TFileWriter();

    //! Opens the writer.
    void Open();

    //! Writes another chunk.
    void Write(const TRef& data);

    //! Closes the writer.
    void Close();

    //! Returns the id of the created node.
    /*!
     *  \note
     *  Can only be called after #Close.
     */
    NCypressClient::TNodeId GetNodeId() const;

private:
    TFileWriterConfigPtr Config;
    NRpc::IChannelPtr MasterChannel;

    NTransactionClient::ITransactionPtr Transaction;
    NTransactionClient::TTransactionManagerPtr TransactionManager;
    NTransactionClient::ITransactionPtr UploadTransaction;
    NYTree::TYPath Path;

    TAutoPtr<TFileChunkOutput> Writer;

    NLog::TTaggedLogger Logger;

    NCypressClient::TNodeId NodeId;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NFileClient
} // namespace NYT
