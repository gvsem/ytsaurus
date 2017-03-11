#pragma once

#include <yt/core/misc/public.h>

namespace ev {

////////////////////////////////////////////////////////////////////////////////

struct dynamic_loop;

////////////////////////////////////////////////////////////////////////////////

} // namespace ev

namespace NYT {
namespace NPipes {

////////////////////////////////////////////////////////////////////////////////

DEFINE_ENUM(EErrorCode,
    ((Aborted) (1500))
);

DECLARE_REFCOUNTED_CLASS(TAsyncReader)
DECLARE_REFCOUNTED_CLASS(TAsyncWriter)
DECLARE_REFCOUNTED_CLASS(TNamedPipe)

namespace NDetail {

DECLARE_REFCOUNTED_CLASS(TAsyncReaderImpl)
DECLARE_REFCOUNTED_CLASS(TAsyncWriterImpl)

} // namespace NDetail

////////////////////////////////////////////////////////////////////////////////

} // namespace NPipes
} // namespace NYT
