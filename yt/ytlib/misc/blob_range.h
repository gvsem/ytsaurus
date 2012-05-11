﻿#pragma once

#include "common.h"
#include "ref.h"

namespace NYT {

///////////////////////////////////////////////////////////////////////////////

class TBlobRange
{
public:
    TBlobRange();
    TBlobRange(const TBlob* blob, size_t offset, size_t length);

    //ToDo(psushin): consider removing this.
    TStringBuf GetStringBuf() const;

    TBlob::const_iterator begin() const;
    size_t size() const;

private:
    const TBlob* Blob;
    size_t Offset;
    size_t Length;
};

bool operator==(const TBlobRange& lhs, const TBlobRange& rhs);

///////////////////////////////////////////////////////////////////////////////

} // namespace NYT

//! A hasher for TBlobRange.
template <>
struct hash<NYT::TBlobRange>
{
    i32 operator()(const NYT::TBlobRange& blobRange) const
    {
        return THash<TStringBuf>()(blobRange.GetStringBuf());
    }
};
