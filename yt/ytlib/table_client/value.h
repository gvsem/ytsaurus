﻿#pragma once

#include "table_chunk_meta.pb.h"

#include <ytlib/misc/common.h>
#include <ytlib/misc/ref.h>
#include <ytlib/misc/nullable.h>

#include <util/stream/mem.h>

namespace NYT {
namespace NTableClient {

////////////////////////////////////////////////////////////////////////////////

//! Array of bytes. The only data type stored inside tables (column names and values).
class TValue
{
public:
    TValue();
    TValue(TRef data);
    TValue(const Stroka& data);

    const char* GetData() const;
    size_t GetSize() const;

    const char* Begin() const;
    const char* End() const;

    bool IsEmpty() const;
    bool IsNull() const;

    int Save(TOutputStream* out);
    static TValue Load(TMemoryInput* in);

    Stroka ToString() const;
    TBlob ToBlob() const;

private:
    TRef Data;
};

bool operator==(const TValue& lhs, const TValue& rhs);
bool operator!=(const TValue& lhs, const TValue& rhs);
bool operator< (const TValue& lhs, const TValue& rhs);
bool operator> (const TValue& lhs, const TValue& rhs);
bool operator<=(const TValue& lhs, const TValue& rhs);
bool operator>=(const TValue& lhs, const TValue& rhs);

////////////////////////////////////////////////////////////////////////////////

typedef std::vector<Stroka> TKey;

bool operator==(const TKey& lhs, const TKey& rhs);
bool operator!=(const TKey& lhs, const TKey& rhs);
bool operator< (const TKey& lhs, const TKey& rhs);
bool operator> (const TKey& lhs, const TKey& rhs);
bool operator<=(const TKey& lhs, const TKey& rhs);
bool operator>=(const TKey& lhs, const TKey& rhs);


////////////////////////////////////////////////////////////////////////////////

} // namespace NTableClient
} // namespace NYT
