#pragma once

#include "public.h"

#include <core/misc/ref.h>

#include <ytlib/new_table_client/chunk_meta.pb.h>

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

class TVariableIterator
{
public:
    TVariableIterator(const char* opaque, int count);

    bool ParseNext(TRowValue* value);
    int GetRemainingCount() const;

private:
    const char* Opaque;
    int Count;

};

////////////////////////////////////////////////////////////////////////////////

class TBlockReader 
{
public:
    TBlockReader(
        const NProto::TBlockMeta& meta,
        const TSharedRef& block,
        const std::vector<EColumnType>& columnTypes);

    void JumpTo(int rowIndex);
    void NextRow();

    bool EndOfBlock() const;
    int GetRowCount() const;

    bool GetEndOfKeyFlag() const;

    // Defines value type based on column types.
    TRowValue Read(int index) const;

    TVariableIterator GetVariableIterator() const;

private:
    struct TColumn
    {
        const char* Begin;
        TDynBitMap NullBitMap;
        EColumnType Type;
    };

    const NProto::TBlockMeta& Meta;
    TSharedRef Block;

    std::vector<TColumn> Columns;
    TDynBitMap EndOfKeyFlags;

    const char* VariableColumn;

    const char* FixedBuffer;
    const char* VariableBuffer;

    int RowIndex;

    int GetVariableColumnCount() const;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
