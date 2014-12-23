#pragma once

#include "public.h"

#include "user_job_io.h"

#include <ytlib/new_table_client/public.h>

#include <ytlib/transaction_client/public.h>

#include <core/logging/log.h>

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////////////////

class TUserJobIOBase
    : public IUserJobIO
{
public:
    TUserJobIOBase(IJobHost* host);

    virtual void Init() override;

    virtual const std::vector<NVersionedTableClient::ISchemalessMultiChunkWriterPtr>& GetWriters() const override;
    virtual const std::vector<NVersionedTableClient::ISchemalessMultiChunkReaderPtr>& GetReaders() const override;

    virtual void PopulateResult(NScheduler::NProto::TSchedulerJobResultExt* schedulerJobResultExt) override;

protected:
    IJobHost* Host_;

    const NScheduler::NProto::TSchedulerJobSpecExt& SchedulerJobSpec_;
    NScheduler::TJobIOConfigPtr JobIOConfig_;

    std::vector<NVersionedTableClient::ISchemalessMultiChunkReaderPtr> Readers_;
    std::vector<NVersionedTableClient::ISchemalessMultiChunkWriterPtr> Writers_;

    NLog::TLogger Logger;

/*

    virtual NTableClient::ISyncWriterUnsafePtr DoCreateWriter(
        NTableClient::TTableWriterOptionsPtr options,
        const NChunkClient::TChunkListId& chunkListId,
        const NTransactionClient::TTransactionId& transactionId) = 0;

    virtual std::vector<NTableClient::ISyncReaderPtr> DoCreateReaders() = 0;

    std::vector<NTableClient::ISyncReaderPtr> CreateRegularReaders(bool isParallel);

    NTableClient::ISyncReaderPtr CreateTableReader(
        NTableClient::TChunkReaderOptionsPtr options,
        std::vector<NChunkClient::NProto::TChunkSpec>& chunkSpecs, 
        bool isParallel);

    NTableClient::ISyncWriterUnsafePtr CreateTableWriter(
        NTableClient::TTableWriterOptionsPtr options,
        const NChunkClient::TChunkListId& chunkListId,
        const NTransactionClient::TTransactionId& transactionId);
    */

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
