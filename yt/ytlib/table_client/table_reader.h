﻿#pragma once
#include "common.h"
#include "chunk_sequence_reader.h"

#include "../rpc/channel.h"
#include "../transaction_client/transaction.h"
#include "../cypress/cypress_service_rpc.h"
#include "../table_server/table_ypath_rpc.h"

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

class TTableReader
    : public TRefCountedBase
{
public:
    typedef TIntrusivePtr<TTableReader> TPtr;

    struct TConfig
        : public TConfigBase
    {
        TDuration CypressRpcTimeout;
        TChunkSequenceReader::TConfig ChunkSequenceReader;

        TConfig()
        {
            Register("cypress_rpc_timeout", CypressRpcTimeout).Default(TDuration::Seconds(5));
            Register("chunk_sequence_reader", ChunkSequenceReader);

            SetDefaults();
        }
    };

    TTableReader(
        const TConfig& config,
        NTransactionClient::ITransaction::TPtr transaction,
        NRpc::IChannel* masterChannel,
        const TChannel& readChannel,
        const Stroka& path);

    bool NextRow();
    bool NextColumn();
    TColumn GetColumn();
    TValue GetValue();
    void Close();

private:
    void OnAborted();

    const TConfig Config;
    TChunkSequenceReader::TPtr Reader;
    NCypress::TNodeId NodeId;

    //! Nullable. Reading doesn't require active transaction.
    NTransactionClient::ITransaction::TPtr Transaction;

    IAction::TPtr OnAborted_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
