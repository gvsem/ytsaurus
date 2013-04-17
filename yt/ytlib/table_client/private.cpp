#include "stdafx.h"
#include "private.h"

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

NLog::TLogger TableReaderLogger("TableReader");
NLog::TLogger TableWriterLogger("TableWriter");

const int DefaultPartitionTag = -1;
const int FormatVersion = 1;

////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT

