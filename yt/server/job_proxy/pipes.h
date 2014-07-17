﻿#pragma once

#include "private.h"
#include <core/misc/blob_output.h>
#include <core/misc/pipe.h>

#include <ytlib/table_client/public.h>

#include <ytlib/pipes/public.h>
#include <ytlib/pipes/async_reader.h>
#include <ytlib/pipes/async_writer.h>

namespace TCLAP {
    class ValueLike;
}

namespace NYT {
namespace NJobProxy {

////////////////////////////////////////////////////////////////////

int SafeDup(int oldFd);
void SafeDup2(int oldFd, int newFd);
int SafePipe(int fd[2]);
void SafeMakeNonblocking(int fd);
void ChmodJobDescriptor(int fd);

// Ensures that descriptor is open and CLOEXEC flag is not set.
void CheckJobDescriptor(int fd);

////////////////////////////////////////////////////////////////////

struct TJobPipe
{
    int PipeIndex;
    int ReadFd;
    int WriteFd;

    typedef TCLAP::ValueLike ValueCategory;
};

Stroka ToString(const TJobPipe& obj);
std::istream& operator>>(std::istream& is, TJobPipe& obj);

/*!
 *  Called from job process after fork and before exec.
 *  Closes unused fds, remaps other to a proper number.
*/
void PrepareReadJobPipe(TJobPipe jobPipe);
void PrepareWriteJobPipe(TJobPipe jobPipe);

////////////////////////////////////////////////////////////////////

struct IDataPipe
    : public virtual TRefCounted
{
    /*!
     *  Called from proxy process after fork.
     *  E.g. makes required pipes non-blocking.
     */
    virtual void PrepareProxyDescriptors() = 0;

    virtual TError DoAll() = 0;

    virtual TError Close() = 0;

    virtual void Finish() = 0;

    virtual TJobPipe GetJobPipe() const = 0;
};

typedef TIntrusivePtr<IDataPipe> IDataPipePtr;

////////////////////////////////////////////////////////////////////

class TOutputPipe
    : public IDataPipe
{
public:
    TOutputPipe(
        int fd[2],
        TOutputStream* output,
        int jobDescriptor);

    virtual void PrepareProxyDescriptors() override;

    virtual TError DoAll() override;

    TError ReadAll();

    virtual TError Close() override;
    virtual void Finish() override;

    virtual TJobPipe GetJobPipe() const override;
private:
    TOutputStream* OutputStream;
    int JobDescriptor;
    TPipe Pipe;

    bool IsFinished;
    bool IsClosed;
    TBlob Buffer;

    NPipes::TAsyncReaderPtr Reader;
};

////////////////////////////////////////////////////////////////////

class TInputPipe
    : public IDataPipe
{
public:
    /*!
     *  \note Takes ownership of the input stream.
     *  \param jobDescriptor - number of underlying read descriptor in the job process.
     */
    TInputPipe(
        int fd[2],
        std::unique_ptr<NTableClient::TTableProducer> tableProducer,
        std::unique_ptr<TBlobOutput> buffer,
        std::unique_ptr<NYson::IYsonConsumer> consumer,
        int jobDescriptor);

    virtual void PrepareProxyDescriptors() override;

    virtual TError DoAll() override;

    TError WriteAll();

    virtual TError Close() override;
    virtual void Finish() override;

    virtual TJobPipe GetJobPipe() const override;
private:
    TPipe Pipe;
    int JobDescriptor;

    std::unique_ptr<NTableClient::TTableProducer> TableProducer;
    std::unique_ptr<TBlobOutput> Buffer;
    std::unique_ptr<NYson::IYsonConsumer> Consumer;
    int Position;

    bool HasData;
    bool IsFinished;

    NPipes::TAsyncWriterPtr Writer;
};

////////////////////////////////////////////////////////////////////

} // namespace NJobProxy
} // namespace NYT
