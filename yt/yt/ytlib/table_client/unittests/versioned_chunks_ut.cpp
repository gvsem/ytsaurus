#include <yt/yt/core/test_framework/framework.h>

#include <yt/yt/ytlib/chunk_client/chunk_reader.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_options.h>
#include <yt/yt/ytlib/chunk_client/chunk_reader_statistics.h>
#include <yt/yt/ytlib/chunk_client/chunk_spec.h>
#include <yt/yt/ytlib/chunk_client/client_block_cache.h>
#include <yt/yt/ytlib/chunk_client/memory_reader.h>
#include <yt/yt/ytlib/chunk_client/memory_writer.h>

#include <yt/yt/ytlib/table_client/cached_versioned_chunk_meta.h>
#include <yt/yt/ytlib/table_client/chunk_state.h>
#include <yt/yt/ytlib/table_client/config.h>
#include <yt/yt/ytlib/table_client/versioned_chunk_reader.h>
#include <yt/yt/ytlib/table_client/versioned_chunk_writer.h>

#include <yt/yt/ytlib/new_table_client/versioned_chunk_reader.h>

#include <yt/yt/ytlib/transaction_client/public.h>

#include <yt/yt/client/table_client/comparator.h>
#include <yt/yt/client/table_client/row_buffer.h>
#include <yt/yt/client/table_client/schema.h>
#include <yt/yt/client/table_client/versioned_reader.h>
#include <yt/yt/client/table_client/versioned_row.h>
#include <yt/yt/client/table_client/versioned_writer.h>

#include <yt/yt/client/table_client/unittests/helpers/helpers.h>

#include <yt/yt/core/compression/public.h>

#include <yt/yt/core/misc/random.h>
#include <yt/yt/core/misc/algorithm_helpers.h>

#include <util/random/shuffle.h>

namespace NYT::NTableClient {
namespace {

using namespace NChunkClient;
using namespace NChunkClient::NProto;
using namespace NTransactionClient;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

const TString A("a");
const TString B("b");

////////////////////////////////////////////////////////////////////////////////

struct TTestOptions
{
    EOptimizeFor OptimizeFor = EOptimizeFor::Scan;
    std::optional<EChunkFormat> ChunkFormat;
    bool UseNewReader = false;
};

TString ToString(const TTestOptions& options)
{
    return Format("%v%v%v",
        options.OptimizeFor,
        options.ChunkFormat ? ToString(*options.ChunkFormat) : "",
        options.UseNewReader ? "New" : "");
}

const auto TestOptionsValues = testing::Values(
    TTestOptions{.OptimizeFor = EOptimizeFor::Scan},
    TTestOptions{.OptimizeFor = EOptimizeFor::Scan, .UseNewReader = true},
    TTestOptions{.OptimizeFor = EOptimizeFor::Lookup},
    TTestOptions{.OptimizeFor = EOptimizeFor::Lookup, .ChunkFormat = EChunkFormat::TableVersionedIndexed},
    TTestOptions{.OptimizeFor = EOptimizeFor::Lookup, .ChunkFormat = EChunkFormat::TableVersionedSlim});

////////////////////////////////////////////////////////////////////////////////

// ToDo(psushin): rewrite this legacy test.
class TVersionedChunkLookupTest
    : public ::testing::Test
    , public testing::WithParamInterface<TTestOptions>
{
protected:
    const TTableSchemaPtr Schema = New<TTableSchema>(std::vector{
        TColumnSchema("k1", EValueType::String)
            .SetSortOrder(ESortOrder::Ascending),
        TColumnSchema("k2", EValueType::Int64)
            .SetSortOrder(ESortOrder::Ascending),
        TColumnSchema("k3", EValueType::Double)
            .SetSortOrder(ESortOrder::Ascending),
        TColumnSchema("v1", EValueType::Int64),
        TColumnSchema("v2", EValueType::Int64)
    });

    IChunkReaderPtr MemoryReader;
    TChunkedMemoryPool MemoryPool;

    void FillKey(
        TMutableVersionedRow row,
        std::optional<TString> k1,
        std::optional<i64> k2,
        std::optional<double> k3)
    {
        row.BeginKeys()[0] = k1
            ? MakeUnversionedStringValue(*k1, 0)
            : MakeUnversionedSentinelValue(EValueType::Null, 0);
        row.BeginKeys()[1] = k2
            ? MakeUnversionedInt64Value(*k2, 1)
            : MakeUnversionedSentinelValue(EValueType::Null, 1);
        row.BeginKeys()[2] = k3
            ? MakeUnversionedDoubleValue(*k3, 2)
            : MakeUnversionedSentinelValue(EValueType::Null, 2);
    }

    TVersionedRow CreateSingleRow(int index)
    {
        auto row = TMutableVersionedRow::Allocate(&MemoryPool, 3, 3, 3, 1);
        FillKey(row, std::make_optional(A), std::make_optional(index), std::nullopt);

        // v1
        row.BeginValues()[0] = MakeVersionedInt64Value(8, 11, 3);
        row.BeginValues()[1] = MakeVersionedInt64Value(7, 3, 3);
        // v2
        row.BeginValues()[2] = MakeVersionedSentinelValue(EValueType::Null, 5, 4);

        row.BeginWriteTimestamps()[2] = 3;
        row.BeginWriteTimestamps()[1] = 5;
        row.BeginWriteTimestamps()[0] = 11;

        row.BeginDeleteTimestamps()[0] = 9;
        return row;
    }

    int CreateManyRows(std::vector<TVersionedRow>* rows, int startIndex)
    {
        const int N = 100000;
        for (int i = 0; i < N; ++i) {
            TVersionedRow row = CreateSingleRow(startIndex + i);
            rows->push_back(row);
        }
        return startIndex + N;
    }

    void WriteManyRows(const TTestOptions& testOptions)
    {
        auto memoryWriter = New<TMemoryWriter>();

        auto config = New<TChunkWriterConfig>();
        config->BlockSize = 1025;
        config->Postprocess();

        auto options = New<TChunkWriterOptions>();
        options->OptimizeFor = testOptions.OptimizeFor;
        options->ChunkFormat = testOptions.ChunkFormat;
        options->Postprocess();

        auto chunkWriter = CreateVersionedChunkWriter(
            config,
            options,
            Schema,
            memoryWriter);

        int startIndex = 0;
        for (int i = 0; i < 3; ++i) {
            std::vector<TVersionedRow> rows;
            startIndex = CreateManyRows(&rows, startIndex);
            chunkWriter->Write(rows);
        }

        EXPECT_TRUE(chunkWriter->Close().Get().IsOK());

        // Initialize reader.
        MemoryReader = CreateMemoryReader(
            memoryWriter->GetChunkMeta(),
            memoryWriter->GetBlocks());
    }

    void DoTest(const TTestOptions& testOptions)
    {
        std::vector<TVersionedRow> expectedRows;
        int startIndex = 0;
        for (int i = 0; i < 3; ++i) {
            std::vector<TVersionedRow> rows;
            startIndex = CreateManyRows(&rows, startIndex);
            expectedRows.insert(expectedRows.end(), rows.begin(), rows.end());
        }

        WriteManyRows(testOptions);

        auto chunkMeta = MemoryReader->GetMeta(/*chunkReadOptions*/ {})
            .Apply(BIND(
                &TCachedVersionedChunkMeta::Create,
                /*prepareColumnarMeta*/ false,
                /*memoryTracker*/ nullptr))
            .Get()
            .ValueOrThrow();

        {
            TChunkedMemoryPool pool;
            TUnversionedOwningRowBuilder builder;

            std::vector<TLegacyOwningKey> owningKeys;

            expectedRows.clear();

            // Before the first key.

            builder.AddValue(MakeUnversionedStringValue(A, 0));
            builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, 1));
            builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, 2));
            owningKeys.push_back(builder.FinishRow());
            expectedRows.push_back(TVersionedRow());

            // The first key.
            builder.AddValue(MakeUnversionedStringValue(A, 0));
            builder.AddValue(MakeUnversionedInt64Value(0, 1));
            builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, 2));
            owningKeys.push_back(builder.FinishRow());

            auto row = TMutableVersionedRow::Allocate(&pool, 3, 1, 1, 1);
            FillKey(row, std::make_optional(A), std::make_optional(0), std::nullopt);
            row.BeginValues()[0] = MakeVersionedInt64Value(8, 11, 3);
            row.BeginWriteTimestamps()[0] = 11;
            row.BeginDeleteTimestamps()[0] = 9;
            expectedRows.push_back(row);

            // Somewhere in the middle.
            builder.AddValue(MakeUnversionedStringValue(A, 0));
            builder.AddValue(MakeUnversionedInt64Value(150000, 1));
            builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, 2));
            owningKeys.push_back(builder.FinishRow());

            row = TMutableVersionedRow::Allocate(&pool, 3, 1, 1, 1);
            FillKey(row, std::make_optional(A), std::make_optional(150000), std::nullopt);
            row.BeginValues()[0] = MakeVersionedInt64Value(8, 11, 3);
            row.BeginWriteTimestamps()[0] = 11;
            row.BeginDeleteTimestamps()[0] = 9;
            expectedRows.push_back(row);

            // After the last key.
            builder.AddValue(MakeUnversionedStringValue(A, 0));
            builder.AddValue(MakeUnversionedInt64Value(350000, 1));
            builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, 2));
            owningKeys.push_back(builder.FinishRow());
            expectedRows.push_back(TVersionedRow());

            // After the previous key, that was after the last.
            builder.AddValue(MakeUnversionedStringValue(B, 0));
            builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, 1));
            builder.AddValue(MakeUnversionedSentinelValue(EValueType::Null, 2));
            owningKeys.push_back(builder.FinishRow());
            expectedRows.push_back(TVersionedRow());

            std::vector<TLegacyKey> keys;
            for (const auto& owningKey : owningKeys) {
                keys.push_back(owningKey);
            }

            auto sharedKeys = MakeSharedRange(std::move(keys), std::move(owningKeys));

            auto chunkState = New<TChunkState>(
                GetNullBlockCache(),
                TChunkSpec(),
                /*chunkMeta*/ nullptr,
                /*overrideTimestamp*/ NullTimestamp,
                /*lookupHashTable*/ nullptr,
                New<TChunkReaderPerformanceCounters>(),
                TKeyComparer(),
                /*virtualValueDirectory*/ nullptr,
                Schema);

            auto chunkReader = testOptions.UseNewReader
                ? NNewTableClient::CreateVersionedChunkReader(
                    sharedKeys,
                    MaxTimestamp,
                    chunkMeta,
                    Schema,
                    TColumnFilter(),
                    /*chunkColumnMapping*/ nullptr,
                    chunkState->BlockCache,
                    TChunkReaderConfig::GetDefault(),
                    MemoryReader,
                    chunkState->PerformanceCounters,
                    /*chunkReadOptions*/ {},
                    /*produceAll*/ false)
                : CreateVersionedChunkReader(
                    TChunkReaderConfig::GetDefault(),
                    MemoryReader,
                    std::move(chunkState),
                    std::move(chunkMeta),
                    /*chunkReadOptions*/ {},
                    sharedKeys,
                    TColumnFilter(),
                    MaxTimestamp,
                    /*produceAll*/ false);

            EXPECT_TRUE(chunkReader->Open().Get().IsOK());
            EXPECT_TRUE(chunkReader->GetReadyEvent().Get().IsOK());

            CheckResult(std::move(expectedRows), chunkReader);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

TEST_P(TVersionedChunkLookupTest, Test)
{
    DoTest(GetParam());
}

INSTANTIATE_TEST_SUITE_P(
    TVersionedChunkLookupTest,
    TVersionedChunkLookupTest,
    TestOptionsValues,
    [] (const auto& info) {
        return ToString(info.param);
    });

////////////////////////////////////////////////////////////////////////////////

TEST_F(TVersionedChunkLookupTest, TestIndexedMetadata)
{
    WriteManyRows({
        .OptimizeFor = EOptimizeFor::Lookup,
        .ChunkFormat = EChunkFormat::TableVersionedIndexed
    });

    auto chunkMeta = MemoryReader->GetMeta(/*chunkReadOptions*/ {})
        .Get()
        .ValueOrThrow();

    auto versionedChunkMeta = TCachedVersionedChunkMeta::Create(
        /*prepareColumnarMeta*/ false,
        /*memoryTracker*/ nullptr,
        chunkMeta);

    auto features = FromProto<EChunkFeatures>(chunkMeta->features());
    EXPECT_TRUE((features & EChunkFeatures::IndexedBlockFormat) == EChunkFeatures::IndexedBlockFormat);

    // TODO(akozhikhov): Check system blocks ext.

    const auto& dataBlockMeta = versionedChunkMeta->DataBlockMeta()->data_blocks(0);
    EXPECT_TRUE(!dataBlockMeta.HasExtension(NProto::TSimpleVersionedBlockMeta::block_meta_ext));
    EXPECT_EQ(1, versionedChunkMeta->Misc().block_format_version());
}

////////////////////////////////////////////////////////////////////////////////

template <class TIter>
void FillRandomUniqueSequence(TFastRng64& rng, TIter begin, TIter end, size_t min, size_t max)
{
    YT_VERIFY(end - begin <= static_cast<ssize_t>(max - min));

    if (begin == end) {
        return;
    }

    YT_VERIFY(min < max);

    TIter current = begin;

    do {
        while (current < end) {
            *current++ = rng.Uniform(min, max);
        }

        std::sort(begin, end);
        current = std::unique(begin, end);
    } while (current < end);
}

TSharedRange<size_t> GetRandomUniqueSequence(TFastRng64& rng, size_t length, size_t min, size_t max)
{
    std::vector<ui64> sequence(length);
    FillRandomUniqueSequence(rng, sequence.begin(), sequence.end(), min, max);
    return MakeSharedRange(std::move(sequence));
}

const TString Letters("ABCDEFGHIJKLMNOPQRSTUVWXYZ");

struct TRandomValueGenerator
{
    TFastRng64& Rng;
    TRowBufferPtr RowBuffer_;
    i64 DomainSize;

    static constexpr int NullRatio = 30;

    TUnversionedValue GetRandomValue(ESimpleLogicalValueType type)
    {
        YT_VERIFY(DomainSize > 0);

        switch (type) {
            case ESimpleLogicalValueType::Int8:
                return MakeUnversionedInt64Value(static_cast<i8>(Rng.Uniform(-DomainSize, DomainSize)));
            case ESimpleLogicalValueType::Int16:
                return MakeUnversionedInt64Value(static_cast<i16>(Rng.Uniform(-DomainSize, DomainSize)));
            case ESimpleLogicalValueType::Int32:
                return MakeUnversionedInt64Value(static_cast<i32>(Rng.Uniform(-DomainSize, DomainSize)));
            case ESimpleLogicalValueType::Interval:
            case ESimpleLogicalValueType::Int64:
                return MakeUnversionedInt64Value(Rng.Uniform(-DomainSize, DomainSize));

            case ESimpleLogicalValueType::Uint8:
                return MakeUnversionedUint64Value(static_cast<ui8>(Rng.Uniform(DomainSize, 2 * DomainSize)));
            case ESimpleLogicalValueType::Uint16:
                return MakeUnversionedUint64Value(static_cast<ui16>(Rng.Uniform(DomainSize, 2 * DomainSize)));
            case ESimpleLogicalValueType::Uint32:
                return MakeUnversionedUint64Value(static_cast<ui32>(Rng.Uniform(DomainSize, 2 * DomainSize)));
            case ESimpleLogicalValueType::Date:
            case ESimpleLogicalValueType::Datetime:
            case ESimpleLogicalValueType::Timestamp:
            case ESimpleLogicalValueType::Uint64:
                return MakeUnversionedUint64Value(Rng.Uniform(DomainSize, 2 * DomainSize));

            case ESimpleLogicalValueType::Utf8:
            case ESimpleLogicalValueType::String: {
                static constexpr int MaxStringLength = 200;
                auto length = Rng.Uniform(std::min<i64>(DomainSize, MaxStringLength));

                char* data = RowBuffer_->GetPool()->AllocateUnaligned(length);
                for (size_t index = 0; index < length; ++index) {
                    data[index] = Letters[Rng.Uniform(Letters.size())];
                }

                return MakeUnversionedStringValue(TStringBuf(data, length));
            }

            case ESimpleLogicalValueType::Json: {
                static constexpr int MaxStringLength = 200;
                auto length = Rng.Uniform(std::min<i64>(DomainSize, MaxStringLength));

                TStringBuilder builder;
                builder.AppendString("{\"key\": \"");
                for (size_t index = 0; index < length; ++index) {
                    builder.AppendChar(Letters[Rng.Uniform(Letters.size())]);
                }
                builder.AppendString("\"}");

                auto payload = builder.Flush();
                auto data = RowBuffer_->GetPool()->AllocateUnaligned(payload.size());
                memcpy(data, payload.data(), payload.size());

                return MakeUnversionedStringValue(TStringBuf(data, payload.size()));
            }

            case ESimpleLogicalValueType::Any: {
                static constexpr int MaxStringLength = 200;
                auto length = Rng.Uniform(std::min<i64>(DomainSize, MaxStringLength));

                TStringBuilder builder;
                builder.AppendString("{key = \"");
                for (size_t index = 0; index < length; ++index) {
                    builder.AppendChar(Letters[Rng.Uniform(Letters.size())]);
                }
                builder.AppendString("\"}");

                auto payload = builder.Flush();
                auto data = RowBuffer_->GetPool()->AllocateUnaligned(payload.size());
                memcpy(data, payload.data(), payload.size());

                return MakeUnversionedAnyValue(TStringBuf(data, payload.size()));
            }

            case ESimpleLogicalValueType::Double:
                return MakeUnversionedDoubleValue(Rng.GenRandReal1() * DomainSize);

            case ESimpleLogicalValueType::Float:
                // NB: Avoid precision issues.
                return MakeUnversionedDoubleValue(Rng.Uniform(65536) / 256.0);

            case ESimpleLogicalValueType::Boolean:
                return MakeUnversionedBooleanValue(static_cast<bool>(Rng.Uniform(2)));

            case ESimpleLogicalValueType::Uuid: {
                static constexpr int Length = 16;

                char* data = RowBuffer_->GetPool()->AllocateUnaligned(Length);
                for (size_t index = 0; index < Length; ++index) {
                    data[index] = static_cast<char>(Rng.Uniform(256));
                }

                return MakeUnversionedStringValue(TStringBuf(data, Length));
            }

            default:
                YT_ABORT();
        }
    }

    TUnversionedValue GetRandomNullableValue(ESimpleLogicalValueType type)
    {
        return Rng.Uniform(NullRatio) > 0
            ? GetRandomValue(type)
            : MakeUnversionedNullValue();
    }
};

////////////////////////////////////////////////////////////////////////////////

class TVersionedChunksHeavyTestBase
    : public ::testing::Test
{
protected:
    const TRowBufferPtr RowBuffer_ = New<TRowBuffer>();

    const std::vector<TColumnSchema> ColumnSchemas_{
        TColumnSchema("k0", EValueType::Int64).SetSortOrder(ESortOrder::Ascending),
        TColumnSchema("k1", EValueType::Uint64).SetSortOrder(ESortOrder::Ascending),
        TColumnSchema("k2", EValueType::String).SetSortOrder(ESortOrder::Ascending),
        TColumnSchema("k3", EValueType::Double).SetSortOrder(ESortOrder::Ascending),
        TColumnSchema("k4", EValueType::Boolean).SetSortOrder(ESortOrder::Ascending),

        TColumnSchema("v1", EValueType::Int64).SetAggregate(TString("min")),
        TColumnSchema("v2", EValueType::Int64),
        TColumnSchema("v3", EValueType::Uint64),
        TColumnSchema("v4", EValueType::String),
        TColumnSchema("v5", EValueType::Double),
        TColumnSchema("v6", EValueType::Boolean)
    };

    std::vector<TString> StringData_;

    const std::vector<TVersionedRow> InitialRows_ = CreateRows();

    TFastRng64 Rng_{42};

    virtual TTestOptions GetTestOptions() = 0;

    IChunkReaderPtr CreateChunk(
        TRange<TVersionedRow> initialRows,
        TTableSchemaPtr writeSchema)
    {
        auto memoryWriter = New<TMemoryWriter>();

        auto testOptions = GetTestOptions();

        auto config = New<TChunkWriterConfig>();
        config->BlockSize = 4_KB;
        config->MaxSegmentValueCount = 128;
        config->SampleRate = 0.0;
        config->Postprocess();

        auto options = New<TChunkWriterOptions>();
        options->OptimizeFor = testOptions.OptimizeFor;
        options->ChunkFormat = testOptions.ChunkFormat;
        options->Postprocess();

        auto chunkWriter = CreateVersionedChunkWriter(
            config,
            options,
            writeSchema,
            memoryWriter);

        chunkWriter->Write(initialRows);
        EXPECT_TRUE(chunkWriter->Close().Get().IsOK());

        return CreateMemoryReader(
            memoryWriter->GetChunkMeta(),
            memoryWriter->GetBlocks());
    }

    void TestRangeReader(
        TRange<TVersionedRow> initialRows,
        IChunkReaderPtr memoryReader,
        TTableSchemaPtr writeSchema,
        TTableSchemaPtr readSchema,
        TSharedRange<TRowRange> ranges,
        TColumnFilter columnFilter,
        TTimestamp timestamp,
        bool produceAllVersions)
    {
        auto expectedRows = CreateExpected(initialRows, writeSchema, readSchema, ranges, timestamp, columnFilter);

        auto chunkMeta = memoryReader->GetMeta(/*chunkReadOptions*/ {})
            .Apply(BIND(
                &TCachedVersionedChunkMeta::Create,
                /*prepareColumnarMeta*/ false,
                /*memoryTracker*/ nullptr))
            .Get()
            .ValueOrThrow();

        auto chunkState = New<TChunkState>(GetNullBlockCache());
        chunkState->TableSchema = readSchema;

        auto chunkReader = GetTestOptions().UseNewReader
            ? NNewTableClient::CreateVersionedChunkReader(
                ranges,
                timestamp,
                chunkMeta,
                readSchema,
                columnFilter,
                nullptr,
                chunkState->BlockCache,
                TChunkReaderConfig::GetDefault(),
                memoryReader,
                chunkState->PerformanceCounters,
                /*chunkReadOptions*/ {},
                produceAllVersions)
            : CreateVersionedChunkReader(
                TChunkReaderConfig::GetDefault(),
                memoryReader,
                std::move(chunkState),
                std::move(chunkMeta),
                /*chunkReadOptions*/ {},
                ranges,
                columnFilter,
                timestamp,
                produceAllVersions);

        CheckResult(std::move(expectedRows), chunkReader);
    }

    void TestRangeReader(
        TRange<TVersionedRow> initialRows,
        IChunkReaderPtr memoryReader,
        TTableSchemaPtr writeSchema,
        TTableSchemaPtr readSchema,
        TLegacyOwningKey lowerKey,
        TLegacyOwningKey upperKey,
        TTimestamp timestamp,
        bool produceAllVersions)
    {
        TestRangeReader(
            initialRows,
            memoryReader,
            writeSchema,
            readSchema,
            MakeSingletonRowRange(lowerKey, upperKey),
            TColumnFilter(),
            timestamp,
            produceAllVersions);
    }

    void TestLookupReader(
        TRange<TVersionedRow> initialRows,
        IChunkReaderPtr memoryReader,
        TTableSchemaPtr writeSchema,
        TTableSchemaPtr readSchema,
        TSharedRange<TUnversionedRow> lookupKeys,
        TColumnFilter columnFilter,
        TTimestamp timestamp,
        bool produceAllVersions)
    {
        auto expectedRows = CreateExpectedRows(initialRows, writeSchema, readSchema, lookupKeys, timestamp, columnFilter);

        auto chunkMeta = memoryReader->GetMeta(/*chunkReadOptions*/ {})
            .Apply(BIND(
                &TCachedVersionedChunkMeta::Create,
                /*prepareColumnarMeta*/ false,
                /*memoryTracker*/ nullptr))
            .Get()
            .ValueOrThrow();

        auto chunkState = New<TChunkState>(GetNullBlockCache());
        chunkState->TableSchema = readSchema;

        auto chunkReader = GetTestOptions().UseNewReader
            ? NNewTableClient::CreateVersionedChunkReader(
                lookupKeys,
                timestamp,
                chunkMeta,
                readSchema,
                columnFilter,
                nullptr,
                chunkState->BlockCache,
                TChunkReaderConfig::GetDefault(),
                memoryReader,
                chunkState->PerformanceCounters,
                /*chunkReadOptions*/ {},
                produceAllVersions)
            : CreateVersionedChunkReader(
                TChunkReaderConfig::GetDefault(),
                memoryReader,
                std::move(chunkState),
                std::move(chunkMeta),
                /*chunkReadOptions*/ {},
                lookupKeys,
                columnFilter,
                timestamp,
                produceAllVersions);

        CheckResult(std::move(expectedRows), chunkReader);
    }

    TString NextStringValue(std::vector<char>& value)
    {
        int index = value.size() - 1;
        while (index >= 0) {
            if (value[index] < 'z') {
                ++value[index];
                return TString(value.data(), value.size());
            } else {
                value[index] = 'a';
                --index;
            }
        }
        YT_ABORT();
    }

    std::vector<TVersionedRow> CreateRows(int rowCount = 10000)
    {
        std::vector<char> stringValue(100, 'a');
        srand(0);

        std::vector<TTimestamp> timestamps = {10, 20, 30, 40, 50, 60, 70, 80, 90};

        std::vector<TVersionedRow> rows;
        rows.reserve(rowCount);
        TVersionedRowBuilder builder(RowBuffer_);

        for (int rowIndex = 0; rowIndex < rowCount; ++rowIndex) {
            builder.AddKey(MakeUnversionedInt64Value(-10 + (rowIndex / 16), 0)); // k0

            builder.AddKey(MakeUnversionedUint64Value((rowIndex / 8) * 128, 1)); // k1

            if (rowIndex / 4 == 0) {
                StringData_.push_back(NextStringValue(stringValue));
            }
            builder.AddKey(MakeUnversionedStringValue(StringData_.back(), 2)); // k2

            builder.AddKey(MakeUnversionedDoubleValue((rowIndex / 2.0) * 3.14, 3)); // k3

            builder.AddKey(MakeUnversionedBooleanValue(rowIndex % 2 == 1, 4)); // k4

            Shuffle(timestamps.begin(), timestamps.end());
            int deleteTimestampCount = std::rand() % 3;
            std::vector<TTimestamp> deleteTimestamps(timestamps.begin(), timestamps.begin() + deleteTimestampCount);
            for (auto timestamp : deleteTimestamps) {
                builder.AddDeleteTimestamp(timestamp);
            }

            std::vector<TTimestamp> writeTimestamps(
                timestamps.begin() + deleteTimestampCount,
                timestamps.begin() + deleteTimestampCount + 3);

            // v1
            for (int i = 0; i < std::rand() % 3; ++i) {
                builder.AddValue(MakeVersionedInt64Value(-10 + (rowIndex / 16) + i, writeTimestamps[i], 5, i % 2 == 0 ? EValueFlags::None : EValueFlags::Aggregate));
            }

            // v2
            for (int i = 0; i < std::rand() % 3; ++i) {
                builder.AddValue(MakeVersionedInt64Value(-10 + (rowIndex / 16) + i, writeTimestamps[i], 6));
            }

            // v3
            for (int i = 0; i < std::rand() % 3; ++i) {
                builder.AddValue(MakeVersionedUint64Value(rowIndex * 10 + i, writeTimestamps[i], 7));
            }

            // v4
            for (int i = 0; i < std::rand() % 3; ++i) {
                builder.AddValue(MakeVersionedStringValue(StringData_.back(), writeTimestamps[i], 8));
            }

            // v5
            for (int i = 0; i < std::rand() % 3; ++i) {
                builder.AddValue(MakeVersionedDoubleValue(rowIndex * 3.14 + i, writeTimestamps[i], 9));
            }

            // v6
            for (int i = 0; i < std::rand() % 3; ++i) {
                builder.AddValue(MakeVersionedBooleanValue(i % 2 == 1, writeTimestamps[i], 10));
            }

            rows.push_back(builder.FinishRow());
        }

        return rows;
    }

    std::vector<int> BuildIdMapping(
        const TTableSchemaPtr& writeSchema,
        const TTableSchemaPtr& readSchema,
        const TColumnFilter& columnFilter)
    {
        std::vector<int> idMapping(writeSchema->GetColumnCount(), -1);
        for (int i = 0; i < writeSchema->GetColumnCount(); ++i) {
            const auto& writeColumnSchema = writeSchema->Columns()[i];
            for (int j = 0; j < readSchema->GetColumnCount(); ++j) {
                const auto& readColumnSchema = readSchema->Columns()[j];
                if (writeColumnSchema.Name() == readColumnSchema.Name() && columnFilter.ContainsIndex(j)) {
                    idMapping[i] = j;
                }
            }
        }
        return idMapping;
    }

    bool CreateExpectedRow(
        TVersionedRowBuilder* builder,
        TVersionedRow row,
        TTimestamp timestamp,
        TRange<int> idMapping,
        TRange<TColumnSchema> readSchemaColumns)
    {
        if (timestamp == AllCommittedTimestamp) {
            for (auto deleteIt = row.BeginDeleteTimestamps(); deleteIt != row.EndDeleteTimestamps(); ++deleteIt) {
                builder->AddDeleteTimestamp(*deleteIt);
            }

            for (auto valueIt = row.BeginValues(); valueIt != row.EndValues(); ++valueIt) {
                if (idMapping[valueIt->Id] > 0) {
                    auto value = *valueIt;
                    value.Id = idMapping[valueIt->Id];
                    builder->AddValue(value);
                }
            }
        } else {
            // Find delete timestamp.
            auto deleteTimestamp = NullTimestamp;
            for (auto deleteIt = row.BeginDeleteTimestamps(); deleteIt != row.EndDeleteTimestamps(); ++deleteIt) {
                if (*deleteIt <= timestamp) {
                    deleteTimestamp = std::max(*deleteIt, deleteTimestamp);
                }
            }
            if (deleteTimestamp != NullTimestamp) {
                builder->AddDeleteTimestamp(deleteTimestamp);
            }

            auto writeTimestamp = NullTimestamp;
            for (auto writeIt = row.BeginWriteTimestamps(); writeIt != row.EndWriteTimestamps(); ++writeIt) {
                if (*writeIt <= timestamp && *writeIt > deleteTimestamp) {
                    writeTimestamp = std::max(*writeIt, writeTimestamp);
                    builder->AddWriteTimestamp(*writeIt);
                }
            }

            if (deleteTimestamp == NullTimestamp && writeTimestamp == NullTimestamp) {
                // Row didn't exist at this timestamp.
                return false;
            }

            // Assume that equal ids are adjacent.
            int lastUsedId = -1;
            for (auto valueIt = row.BeginValues(); valueIt != row.EndValues(); ++valueIt) {
                if (idMapping[valueIt->Id] > 0 && valueIt->Timestamp <= timestamp && valueIt->Timestamp > deleteTimestamp) {
                    auto targetId = idMapping[valueIt->Id];

                    if (targetId != lastUsedId || readSchemaColumns[targetId].Aggregate()) {
                        auto value = *valueIt;
                        value.Id = targetId;
                        builder->AddValue(value);
                        lastUsedId = targetId;
                    }
                }
            }
        }

        return true;
    }

    std::vector<TVersionedRow> CreateExpected(
        TRange<TVersionedRow> rows,
        const TTableSchemaPtr& writeSchema,
        const TTableSchemaPtr& readSchema,
        const TSharedRange<TRowRange>& ranges,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter)
    {
        YT_VERIFY(writeSchema->GetKeyColumnCount() <= readSchema->GetKeyColumnCount());

        if (ranges.empty()) {
            return {};
        }

        std::vector<TVersionedRow> expectedRows;

        auto idMapping = BuildIdMapping(writeSchema, readSchema, columnFilter);

        const auto* currentRange = ranges.begin();

        std::vector<EValueType> columnWireTypes;
        columnWireTypes.reserve(readSchema->GetKeyColumnCount());
        for (int i = 0; i < readSchema->GetKeyColumnCount(); ++i) {
            columnWireTypes.push_back(readSchema->Columns()[i].GetWireType());
        }

        std::vector<TUnversionedValue> key(readSchema->GetKeyColumnCount());

        for (int i = 0; i < readSchema->GetKeyColumnCount(); ++i) {
            key[i] = MakeUnversionedSentinelValue(EValueType::Null, i);
        }

        TVersionedRowBuilder builder(RowBuffer_, timestamp == AllCommittedTimestamp);

        for (auto row : rows) {
            YT_VERIFY(row.GetKeyCount() <= readSchema->GetKeyColumnCount());
            for (int i = 0; i < row.GetKeyCount() && i < readSchema->GetKeyColumnCount(); ++i) {
                auto actualType = row.BeginKeys()[i].Type;
                YT_VERIFY(actualType == columnWireTypes[i] || actualType == EValueType::Null);

                key[i] = row.BeginKeys()[i];
            }

            while (CompareValueRanges(MakeRange(key), currentRange->second.Elements()) >= 0) {
                ++currentRange;
                if (currentRange == ranges.end()) {
                    return expectedRows;
                }
            }

            if (CompareValueRanges(MakeRange(key), currentRange->first.Elements()) < 0) {
                continue;
            }

            for (const auto& value : key) {
                builder.AddKey(value);
            }

            if (CreateExpectedRow(&builder, row, timestamp, idMapping, readSchema->Columns())) {
                expectedRows.push_back(builder.FinishRow());
            } else {
                // Finish row to reset builder.
                builder.FinishRow();
                expectedRows.push_back(TVersionedRow());
            }
        }

        return expectedRows;
    }

    std::vector<TVersionedRow> CreateExpectedRows(
        TRange<TVersionedRow> rows,
        const TTableSchemaPtr& writeSchema,
        const TTableSchemaPtr& readSchema,
        const TSharedRange<TUnversionedRow>& keys,
        TTimestamp timestamp,
        const TColumnFilter& columnFilter)
    {
        YT_VERIFY(writeSchema->GetKeyColumnCount() <= readSchema->GetKeyColumnCount());
        int keyColumnCount = writeSchema->GetKeyColumnCount();

        std::vector<TVersionedRow> expectedRows;
        expectedRows.reserve(keys.size());

        auto idMapping = BuildIdMapping(writeSchema, readSchema, columnFilter);

        auto rowsIt = rows.begin();

        TVersionedRowBuilder builder(RowBuffer_, timestamp == AllCommittedTimestamp);

        for (auto key : keys) {
            rowsIt = ExponentialSearch(rowsIt, rows.end(), [&] (auto rowsIt) {
                return CompareValueRanges(rowsIt->Keys(), key.FirstNElements(keyColumnCount)) < 0;
            });

            bool nullPadding = true;
            for (auto paddedKeyValue = key.Begin() + keyColumnCount; paddedKeyValue != key.End(); ++paddedKeyValue) {
                if (paddedKeyValue->Type != EValueType::Null) {
                    nullPadding = false;
                }
            }

            if (rowsIt != rows.end() &&
                nullPadding &&
                CompareValueRanges(rowsIt->Keys(), key.FirstNElements(keyColumnCount)) == 0)
            {
                for (const auto& value : key) {
                    builder.AddKey(value);
                }

                if (CreateExpectedRow(&builder, *rowsIt, timestamp, idMapping, readSchema->Columns())) {
                    expectedRows.push_back(builder.FinishRow());
                } else {
                    // Finish row to reset builder.
                    builder.FinishRow();
                    expectedRows.push_back(TVersionedRow());
                }
            } else {
                expectedRows.push_back(TVersionedRow());
            }
        }

        return expectedRows;
    }

    void DoFullScanCompaction()
    {
        auto writeSchema = New<TTableSchema>(ColumnSchemas_);
        auto readSchema = New<TTableSchema>(ColumnSchemas_);

        auto memoryChunkReader = CreateChunk(
            InitialRows_,
            writeSchema);

        TestRangeReader(
            InitialRows_,
            memoryChunkReader,
            writeSchema,
            readSchema,
            MinKey(),
            MaxKey(),
            AllCommittedTimestamp,
            /*produceAllVersions*/ true);
    }

    void DoTimestampFullScanExtraKeyColumn(TTimestamp timestamp)
    {
        auto writeSchema = New<TTableSchema>(ColumnSchemas_);

        auto memoryChunkReader = CreateChunk(
            InitialRows_,
            writeSchema);

        auto columnSchemas = ColumnSchemas_;
        columnSchemas.insert(
            columnSchemas.begin() + 5,
            TColumnSchema("extraKey", EValueType::Boolean).SetSortOrder(ESortOrder::Ascending));

        auto readSchema = New<TTableSchema>(columnSchemas);

        TestRangeReader(
            InitialRows_,
            memoryChunkReader,
            writeSchema,
            readSchema,
            MinKey(),
            MaxKey(),
            timestamp,
            /*produceAllVersions*/ false);
    }

    void DoEmptyReadWideSchema()
    {
        auto writeSchema = New<TTableSchema>(ColumnSchemas_);

        auto memoryChunkReader = CreateChunk(
            InitialRows_,
            writeSchema);

        auto columnSchemas = ColumnSchemas_;
        columnSchemas.insert(
            columnSchemas.begin() + 5,
            TColumnSchema("extraKey", EValueType::Boolean).SetSortOrder(ESortOrder::Ascending));
        auto readSchema = New<TTableSchema>(columnSchemas);

        TUnversionedOwningRowBuilder lowerKeyBuilder;
        for (auto it = InitialRows_[1].BeginKeys(); it != InitialRows_[1].EndKeys(); ++it) {
            lowerKeyBuilder.AddValue(*it);
        }
        lowerKeyBuilder.AddValue(MakeUnversionedBooleanValue(false));
        auto lowerKey = lowerKeyBuilder.FinishRow();

        TUnversionedOwningRowBuilder upperKeyBuilder;
        for (auto it = InitialRows_[1].BeginKeys(); it != InitialRows_[1].EndKeys(); ++it) {
            upperKeyBuilder.AddValue(*it);
        }
        upperKeyBuilder.AddValue(MakeUnversionedBooleanValue(true));
        auto upperKey = upperKeyBuilder.FinishRow();

        TestRangeReader(
            InitialRows_,
            memoryChunkReader,
            writeSchema,
            readSchema,
            lowerKey,
            upperKey,
            /*timestamp*/ 25,
            /*produceAllVersions*/ false);
    }

    void DoGroupsLimitsAndSchemaChange()
    {
        auto writeColumnSchemas = ColumnSchemas_;
        writeColumnSchemas[5].SetGroup(TString("G1"));
        writeColumnSchemas[9].SetGroup(TString("G1"));

        writeColumnSchemas[6].SetGroup(TString("G2"));
        writeColumnSchemas[7].SetGroup(TString("G2"));
        writeColumnSchemas[8].SetGroup(TString("G2"));
        auto writeSchema = New<TTableSchema>(writeColumnSchemas);

        auto readColumnSchemas = ColumnSchemas_;
        readColumnSchemas.erase(readColumnSchemas.begin() + 7, readColumnSchemas.end());
        readColumnSchemas.insert(readColumnSchemas.end(), TColumnSchema("extraValue", EValueType::Boolean));
        auto readSchema = New<TTableSchema>(readColumnSchemas);

        auto lowerRowIndex = InitialRows_.size() / 3;
        auto lowerKey = ToOwningKey(InitialRows_[lowerRowIndex]);

        auto upperRowIndex = InitialRows_.size() - 1;
        auto upperKey = ToOwningKey(InitialRows_[upperRowIndex]);

        auto memoryChunkReader = CreateChunk(
            InitialRows_,
            writeSchema);

        TestRangeReader(
            InitialRows_,
            memoryChunkReader,
            writeSchema,
            readSchema,
            lowerKey,
            upperKey,
            SyncLastCommittedTimestamp,
            /*produceAllVersions*/ false);
    }
};

////////////////////////////////////////////////////////////////////////////////

// Good combination to test all types of segments.
const std::vector<EValueType> KeyTypes{
    EValueType::Int64,
    EValueType::Double,
    EValueType::Uint64,
    EValueType::String,
    EValueType::Boolean
};

////////////////////////////////////////////////////////////////////////////////

class TVersionedChunksHeavyTest
    : public TVersionedChunksHeavyTestBase
    , public testing::WithParamInterface<TTestOptions>
{
private:
    TTestOptions GetTestOptions() override
    {
        return GetParam();
    }
};

TEST_P(TVersionedChunksHeavyTest, FullScanCompaction)
{
    DoFullScanCompaction();
}

TEST_P(TVersionedChunksHeavyTest, TimestampFullScanExtraKeyColumn)
{
    DoTimestampFullScanExtraKeyColumn(/*timestamp*/ 50);
}

TEST_P(TVersionedChunksHeavyTest, TimestampFullScanExtraKeyColumnSyncLastCommitted)
{
    DoTimestampFullScanExtraKeyColumn(SyncLastCommittedTimestamp);
}

TEST_P(TVersionedChunksHeavyTest, TimestampFullScanExtraKeyColumnScanSyncLastCommittedNew)
{
    DoTimestampFullScanExtraKeyColumn(SyncLastCommittedTimestamp);
}

TEST_P(TVersionedChunksHeavyTest, GroupsLimitsAndSchemaChange)
{
    DoGroupsLimitsAndSchemaChange();
}

TEST_P(TVersionedChunksHeavyTest, EmptyReadWideSchemaScan)
{
    DoEmptyReadWideSchema();
}

INSTANTIATE_TEST_SUITE_P(
    TVersionedChunksHeavyTest,
    TVersionedChunksHeavyTest,
    TestOptionsValues,
    [] (const auto& info) {
        return ToString(info.param);
    });

////////////////////////////////////////////////////////////////////////////////

class TVersionedChunksHeavyWithBoundsTest
    : public TVersionedChunksHeavyTestBase
    , public testing::WithParamInterface<std::tuple<
        TTestOptions,
        std::tuple<
            EValueType,
            EValueType
        >
    >>
{
public:
    void DoReadWideSchemaWithBounds()
    {
        auto [lowerBound, upperBound] = GetBounds();

        auto writeSchema = New<TTableSchema>(ColumnSchemas_);

        auto memoryChunkReader = CreateChunk(
            InitialRows_,
            writeSchema);

        auto columnSchemas = ColumnSchemas_;
        columnSchemas.insert(
            columnSchemas.begin() + 5,
            TColumnSchema("extraKey", EValueType::Int64).SetSortOrder(ESortOrder::Ascending));
        auto readSchema = New<TTableSchema>(columnSchemas);

        TUnversionedOwningRowBuilder lowerKeyBuilder;
        for (auto it = InitialRows_[1].BeginKeys(); it != InitialRows_[1].EndKeys(); ++it) {
            lowerKeyBuilder.AddValue(*it);
        }
        lowerKeyBuilder.AddValue(lowerBound);
        auto lowerKey = lowerKeyBuilder.FinishRow();

        TUnversionedOwningRowBuilder upperKeyBuilder;
        for (auto it = InitialRows_[1].BeginKeys(); it != InitialRows_[1].EndKeys(); ++it) {
            upperKeyBuilder.AddValue(*it);
        }
        upperKeyBuilder.AddValue(upperBound);
        auto upperKey = upperKeyBuilder.FinishRow();

        TestRangeReader(
            InitialRows_,
            memoryChunkReader,
            writeSchema,
            readSchema,
            lowerKey,
            upperKey,
            /*timestamp*/ 25,
            /*produceAllVersions*/ false);
    }

private:
    std::tuple<TUnversionedValue, TUnversionedValue> GetBounds()
    {
        auto [lowerSentinel, upperSentinel] = std::get<1>(GetParam());
        return {MakeUnversionedSentinelValue(lowerSentinel), MakeUnversionedSentinelValue(upperSentinel)};
    }

    TTestOptions GetTestOptions() override
    {
        return std::get<0>(GetParam());
    }
};

TEST_P(TVersionedChunksHeavyWithBoundsTest, ReadWideSchemaWithNonscalarBounds)
{
    DoReadWideSchemaWithBounds();
}

INSTANTIATE_TEST_SUITE_P(
    TVersionedChunksHeavyWithBoundsTest,
    TVersionedChunksHeavyWithBoundsTest,
    ::testing::Combine(
        TestOptionsValues,
        ::testing::Values(
            std::make_pair(EValueType::Min, EValueType::Min),
            std::make_pair(EValueType::Min, EValueType::Null),
            std::make_pair(EValueType::Min, EValueType::Max),
            std::make_pair(EValueType::Null, EValueType::Null),
            std::make_pair(EValueType::Null, EValueType::Max),
            std::make_pair(EValueType::Max, EValueType::Max))),
    [] (const auto& info) {
        return Format("%v_%kv_%kv",
            std::get<0>(info.param),
            std::get<0>(std::get<1>(info.param)), std::get<1>(std::get<1>(info.param)));
    });

////////////////////////////////////////////////////////////////////////////////

class TVersionedChunksStressTest
    : public TVersionedChunksHeavyTestBase
    , public testing::WithParamInterface<std::tuple<
        TTestOptions,
        /*AreColumnGroupsEnabled*/ bool
    >>
{
protected:
    TTableSchemaPtr WriteSchema_;
    TTableSchemaPtr ReadSchema_;

    bool AreColumnGroupsEnabled()
    {
        return std::get<1>(GetParam());
    }

    TTestOptions GetTestOptions() override
    {
        return std::get<0>(GetParam());
    }

    void ProduceSchemas()
    {
        auto keyColumnCount = std::ssize(KeyTypes);
        std::vector<TColumnSchema> writeSchemaColumns;

        // Keys
        for (int index = 0; index < keyColumnCount; ++index) {
            writeSchemaColumns.push_back(TColumnSchema(Format("k%v", index), KeyTypes[index])
                .SetSortOrder(ESortOrder::Ascending));
        }

        // Values
        writeSchemaColumns.push_back(TColumnSchema("v01", EValueType::Int64).SetAggregate(TString("min")));
        writeSchemaColumns.push_back(TColumnSchema("v02", EValueType::Int64));
        writeSchemaColumns.push_back(TColumnSchema("v03", EValueType::Uint64));
        writeSchemaColumns.push_back(TColumnSchema("v04", EValueType::String));
        writeSchemaColumns.push_back(TColumnSchema("v05", EValueType::Double));
        writeSchemaColumns.push_back(TColumnSchema("v06", EValueType::Boolean));
        writeSchemaColumns.push_back(TColumnSchema("v07", EValueType::Any));
        // TODO(babenko): uncomment once these types are fully supported by all readers/writers
        // writeSchemaColumns.push_back(TColumnSchema("v08", ESimpleLogicalValueType::Int8));
        // writeSchemaColumns.push_back(TColumnSchema("v09", ESimpleLogicalValueType::Int16));
        // writeSchemaColumns.push_back(TColumnSchema("v10", ESimpleLogicalValueType::Int32));
        // writeSchemaColumns.push_back(TColumnSchema("v11", ESimpleLogicalValueType::Uint8));
        // writeSchemaColumns.push_back(TColumnSchema("v12", ESimpleLogicalValueType::Uint16));
        // writeSchemaColumns.push_back(TColumnSchema("v13", ESimpleLogicalValueType::Uint32));
        // writeSchemaColumns.push_back(TColumnSchema("v14", ESimpleLogicalValueType::Utf8));
        // writeSchemaColumns.push_back(TColumnSchema("v15", ESimpleLogicalValueType::Date));
        // writeSchemaColumns.push_back(TColumnSchema("v16", ESimpleLogicalValueType::Datetime));
        // writeSchemaColumns.push_back(TColumnSchema("v17", ESimpleLogicalValueType::Timestamp));
        // writeSchemaColumns.push_back(TColumnSchema("v18", ESimpleLogicalValueType::Interval));
        // writeSchemaColumns.push_back(TColumnSchema("v19", ESimpleLogicalValueType::Float));
        // writeSchemaColumns.push_back(TColumnSchema("v20", ESimpleLogicalValueType::Json));
        // writeSchemaColumns.push_back(TColumnSchema("v21", ESimpleLogicalValueType::Uuid));
        // TODO(babenko): test Void type

        if (AreColumnGroupsEnabled()) {
            writeSchemaColumns[2].SetGroup("a");
            writeSchemaColumns[4].SetGroup("a");
            writeSchemaColumns[5].SetGroup("b");
        }

        auto readSchemaColumns = writeSchemaColumns;
        readSchemaColumns.insert(
            readSchemaColumns.begin() + keyColumnCount,
            TColumnSchema(Format("k%v", keyColumnCount), EValueType::Int64)
                .SetSortOrder(ESortOrder::Ascending));

        readSchemaColumns.push_back(TColumnSchema("x1", EValueType::Double));
        readSchemaColumns.push_back(TColumnSchema("x2", EValueType::Uint64));
        std::swap(readSchemaColumns[keyColumnCount + 1], readSchemaColumns[keyColumnCount + 5]);
        std::swap(readSchemaColumns[keyColumnCount + 2], readSchemaColumns[keyColumnCount + 8]);

        WriteSchema_ = New<TTableSchema>(writeSchemaColumns);
        ReadSchema_ = New<TTableSchema>(readSchemaColumns);
    }

    void DoStressTest()
    {
        ProduceSchemas();

        int distinctValueCount = 10;
        for (i64 valueDomainSize : {10, 100, 100000000}) {
            // Generate random values for each type.
            TRandomValueGenerator valueGenerator{Rng_, RowBuffer_, valueDomainSize};
            TRadomKeyGenerator keyGenerator(&valueGenerator, distinctValueCount);
            auto rows = GenerateRandomRows(&valueGenerator, &keyGenerator);

            auto memoryChunkReader = CreateChunk(
                rows,
                WriteSchema_);

            for (auto readSchema : {WriteSchema_, ReadSchema_}) {
                for (auto generateColumnFilter : {false, true}) {
                    for (auto timestamp : {TTimestamp(50), AllCommittedTimestamp}) {
                        if (generateColumnFilter && timestamp == AllCommittedTimestamp) {
                            continue;
                        }

                        StressTestLookup(
                            memoryChunkReader,
                            timestamp,
                            &keyGenerator,
                            readSchema,
                            rows,
                            generateColumnFilter);

                        StressTestRangesWithCommonPrefix(
                            memoryChunkReader,
                            timestamp,
                            &keyGenerator,
                            readSchema,
                            rows,
                            generateColumnFilter);

                        StressTestArbitraryRanges(
                            memoryChunkReader,
                            timestamp,
                            &keyGenerator,
                            &valueGenerator,
                            readSchema,
                            rows,
                            generateColumnFilter);
                    }
                }
            }

            RowBuffer_->Clear();
        }
    }

private:
    static TUnversionedValue MakeUnversionedValue(
        TUnversionedValue value,
        ui16 id)
    {
        value.Id = id;
        return value;
    }

    static TVersionedValue MakeVersionedValue(
        TUnversionedValue value,
        TTimestamp timestamp,
        ui16 id,
        EValueFlags flags = EValueFlags::None)
    {
        TVersionedValue result;
        static_cast<TUnversionedValue&>(result) = value;
        result.Timestamp = timestamp;
        result.Id = id;
        result.Flags = flags;
        return result;
    }

    static std::vector<TUnversionedValue> GenerateRandomValues(
        ESimpleLogicalValueType type,
        TRandomValueGenerator* valueGenerator,
        int distinctValueCount)
    {
        std::vector<TUnversionedValue> values;
        values.reserve(distinctValueCount + 1);

        values.push_back(MakeUnversionedSentinelValue(EValueType::Null));
        for (int index = 0; index < distinctValueCount; ++index) {
            values.push_back(valueGenerator->GetRandomValue(type));
        }

        SortUnique(values);

        return values;
    }

    struct TRadomKeyGenerator
    {
        TRadomKeyGenerator(
            TRandomValueGenerator* valueGenerator,
            int distinctValueCount)
        {
            for (auto keyType : KeyTypes) {
                auto logicalKeyType = GetLogicalType(keyType);
                GetValues(keyType) = GenerateRandomValues(logicalKeyType, valueGenerator, distinctValueCount);
                GetValues(keyType) = GenerateRandomValues(logicalKeyType, valueGenerator, distinctValueCount);
                GetValues(keyType) = GenerateRandomValues(logicalKeyType, valueGenerator, distinctValueCount);
                GetValues(keyType) = GenerateRandomValues(logicalKeyType, valueGenerator, distinctValueCount);
                GetValues(keyType) = GenerateRandomValues(logicalKeyType, valueGenerator, distinctValueCount);
            }
        }

        int GetDistinctKeyCount()
        {
            int result = 1;
            for (auto type : KeyTypes) {
                result *= std::ssize(GetValues(type));
            }
            return result;
        }

        std::vector<TUnversionedValue>& GetValues(EValueType type)
        {
            switch (type) {
                case EValueType::Int64:
                    return IntValues_;
                case EValueType::Uint64:
                    return UintValues_;
                case EValueType::String:
                    return StringValues_;
                case EValueType::Double:
                    return DoubleValues_;
                case EValueType::Boolean:
                    return BooleanValues_;
                default:
                    YT_ABORT();
            }
        }

        std::vector<TUnversionedValue> ValueStack;

        void Generate(TVersionedRowBuilder* builder, size_t index)
        {
            ValueStack.clear();

            for (int id = KeyTypes.size() - 1; id >= 0; --id) {
                const auto& values = GetValues(KeyTypes[id]);

                auto value = values[index % values.size()];
                index /= values.size();
                value.Id = id;

                ValueStack.push_back(value);
            }

            while (!ValueStack.empty()) {
                builder->AddKey(ValueStack.back());
                ValueStack.pop_back();
            }
        }

        void Generate(
            TUnversionedRowBuilder* builder,
            size_t index,
            const TTableSchemaPtr& schema,
            bool padWithNulls)
        {
            ValueStack.clear();

            for (int id = schema->GetKeyColumnCount() - 1; id >= 0; --id) {
                const auto& values = GetValues(schema->Columns()[id].GetWireType());

                if (id >= std::ssize(KeyTypes)) {
                    if (padWithNulls) {
                        ValueStack.push_back(MakeUnversionedNullValue(id));
                    } else {
                        ValueStack.push_back(values[index % values.size()]);
                        ValueStack.back().Id = id;
                    }
                    continue;
                }

                auto value = values[index % values.size()];
                index /= values.size();
                value.Id = id;

                ValueStack.push_back(value);
            }

            while (!ValueStack.empty()) {
                builder->AddValue(ValueStack.back());
                ValueStack.pop_back();
            }
        }

        void GenerateBound(
            TUnversionedRowBuilder* builder,
            TRandomValueGenerator* valueGenerator,
            TFastRng64* rng)
        {
            auto prefixSize = rng->Uniform(12);

            if (prefixSize > 8) {
                prefixSize = 5;
            } else if (prefixSize > 4) {
                prefixSize = 4;
            } else if (prefixSize > 2) {
                prefixSize = 3;
            } else if (prefixSize > 1) {
                prefixSize = 2;
            } else if (prefixSize > 0) {
                prefixSize = 1;
            } else {
                prefixSize = 0;
            }

            for (size_t id = 0; id < std::min(prefixSize, KeyTypes.size()); ++id) {
                auto type = GetLogicalType(KeyTypes[id]);
                builder->AddValue(MakeUnversionedValue(valueGenerator->GetRandomNullableValue(type), id));
            }

            auto sentinel = rng->Uniform(7);
            if (sentinel == 0) {
                builder->AddValue(MakeUnversionedSentinelValue(EValueType::Min));
            }

            if (sentinel == 1) {
                builder->AddValue(MakeUnversionedSentinelValue(EValueType::Max));
            }
        }

        private:
            std::vector<TUnversionedValue> IntValues_;
            std::vector<TUnversionedValue> UintValues_;
            std::vector<TUnversionedValue> StringValues_;
            std::vector<TUnversionedValue> DoubleValues_;
            std::vector<TUnversionedValue> BooleanValues_;
    };

    // Greater value domain size for direct segment encoding.
    std::vector<TVersionedRow> GenerateRandomRows(
        TRandomValueGenerator* valueGenerator,
        TRadomKeyGenerator* keyGenerator)
    {
        auto distinctKeyCount = keyGenerator->GetDistinctKeyCount();

        auto randomUniqueSequence = GetRandomUniqueSequence(
            Rng_,
            Rng_.Uniform(distinctKeyCount / 7, distinctKeyCount / 3),
            0,
            distinctKeyCount);

        TVersionedRowBuilder builder(RowBuffer_);

        std::vector<TVersionedRow> rows;
        rows.reserve(std::ssize(randomUniqueSequence));

        std::vector<TTimestamp> timestamps{10, 20, 30, 40, 50, 60, 70, 80, 90};

        constexpr int SegmentModeSwitchLimit = 500;
        int rowIndex = 0;

        for (auto index : randomUniqueSequence) {
            // Generate random rows selecting value by index from generated.
            keyGenerator->Generate(&builder, index);

            Shuffle(timestamps.begin(), timestamps.end());
            int deleteTimestampCount = Rng_.Uniform(4);
            std::vector<TTimestamp> deleteTimestamps(timestamps.begin(), timestamps.begin() + deleteTimestampCount);
            for (auto timestamp : deleteTimestamps) {
                builder.AddDeleteTimestamp(timestamp);
            }

            std::vector<TTimestamp> writeTimestamps(
                timestamps.begin() + deleteTimestampCount,
                timestamps.begin() + deleteTimestampCount + 4);

            std::vector<bool> sparseColumns(WriteSchema_->GetColumnCount());
            if (rowIndex % SegmentModeSwitchLimit == 0) {
                for (int id = WriteSchema_->GetKeyColumnCount(); id < WriteSchema_->GetColumnCount(); ++id) {
                    sparseColumns[id] = static_cast<bool>(Rng_.Uniform(2));
                }
            }
            ++rowIndex;

            for (int id = WriteSchema_->GetKeyColumnCount(); id < WriteSchema_->GetColumnCount(); ++id) {
                auto count = [&] {
                    if (sparseColumns[id]) {
                        auto count = static_cast<int>(Rng_.Uniform(12));
                        if (count > 3) {
                            count = 0;
                        }
                        return count;
                    } else {
                        return static_cast<int>(Rng_.Uniform(4));
                    }
                }();
                const auto& columnSchema = WriteSchema_->Columns()[id];
                auto type = columnSchema.CastToV1Type();
                bool aggregate = columnSchema.Name() == "v01";
                for (int i = 0; i < count; ++i) {
                    builder.AddValue(MakeVersionedValue(
                        valueGenerator->GetRandomValue(type),
                        writeTimestamps[i],
                        id,
                        aggregate ? EValueFlags::Aggregate : EValueFlags::None));
                }
            }

            auto row = builder.FinishRow();

            if (row.GetValueCount() > 0) {
                rows.push_back(row);
            }
        }

        return rows;
    }

    TColumnFilter GenerateColumnFilter(const TTableSchemaPtr& readSchema)
    {
        std::vector<int> indexes;
        for (int columnIndex = 0; columnIndex < readSchema->GetColumnCount(); ++columnIndex) {
            if (Rng_.Uniform(2) == 0) {
                indexes.push_back(columnIndex);
            }
        }
        return TColumnFilter{indexes};
    }

    void StressTestLookup(
        IChunkReaderPtr memoryReader,
        TTimestamp timestamp,
        TRadomKeyGenerator* keyGenerator,
        const TTableSchemaPtr& readSchema,
        const std::vector<TVersionedRow>& rows,
        bool generateColumnFilter)
    {
        // Test lookup keys.
        for (int keyCount : {1, 2, 3, 10, 100, 200, 5000}) {
            TUnversionedRowBuilder builder;

            auto randomUniqueSequence = GetRandomUniqueSequence(
                Rng_,
                std::min(keyCount, keyGenerator->GetDistinctKeyCount() * 2 / 3),
                0,
                keyGenerator->GetDistinctKeyCount());

            std::vector<TUnversionedRow> lookupKeys;
            for (auto index : randomUniqueSequence) {
                builder.Reset();
                keyGenerator->Generate(&builder, index, readSchema, /*padWithNulls*/ lookupKeys.size() % 2 == 0);
                lookupKeys.push_back(RowBuffer_->CaptureRow(builder.GetRow(), false));
            }

            auto columnFilter = generateColumnFilter
                ? GenerateColumnFilter(readSchema)
                : TColumnFilter();

            TestLookupReader(
                rows,
                memoryReader,
                WriteSchema_,
                readSchema,
                MakeSharedRange(lookupKeys, RowBuffer_),
                columnFilter,
                timestamp,
                timestamp == AllCommittedTimestamp);
        }
    }

    void StressTestRangesWithCommonPrefix(
        IChunkReaderPtr memoryReader,
        TTimestamp timestamp,
        TRadomKeyGenerator* keyGenerator,
        const TTableSchemaPtr& readSchema,
        const std::vector<TVersionedRow>& rows,
        bool generateColumnFilter)
    {
        for (int keyCount : {2, 4, 7, 30, 100, 200, 5000}) {
            TUnversionedRowBuilder builder;

            auto randomUniqueSequence = GetRandomUniqueSequence(
                Rng_,
                std::min(keyCount, keyGenerator->GetDistinctKeyCount() * 2 / 3),
                0,
                keyGenerator->GetDistinctKeyCount());

            std::vector<TUnversionedRow> lookupKeys;
            for (auto index : randomUniqueSequence) {
                builder.Reset();
                keyGenerator->Generate(&builder, index, WriteSchema_, false);

                auto sentinel = Rng_.Uniform(7);
                if (sentinel == 0) {
                    builder.AddValue(MakeUnversionedSentinelValue(EValueType::Min));
                }

                if (sentinel == 1) {
                    builder.AddValue(MakeUnversionedSentinelValue(EValueType::Max));
                }

                lookupKeys.push_back(RowBuffer_->CaptureRow(builder.GetRow(), false));
            }

            std::vector<TRowRange> readRanges;
            for (int rangeIndex = 0; rangeIndex < std::ssize(lookupKeys) / 2; ++rangeIndex) {
                readRanges.push_back(TRowRange(lookupKeys[2 * rangeIndex], lookupKeys[2 * rangeIndex + 1]));
            }

            if (readRanges.empty()) {
                continue;
            }

            auto columnFilter = generateColumnFilter
                ? GenerateColumnFilter(readSchema)
                : TColumnFilter();

            TestRangeReader(
                rows,
                memoryReader,
                WriteSchema_,
                readSchema,
                MakeSharedRange(readRanges, RowBuffer_),
                columnFilter,
                timestamp,
                timestamp == AllCommittedTimestamp);
        }
    }

    void StressTestArbitraryRanges(
        IChunkReaderPtr memoryReader,
        TTimestamp timestamp,
        TRadomKeyGenerator* keyGenerator,
        TRandomValueGenerator* valueGenerator,
        const TTableSchemaPtr& readSchema,
        const std::vector<TVersionedRow>& rows,
        bool generateColumnFilter)
    {
        for (i64 rangeCount : {1, 2, 3, 13, 29, 30, 36, 37, 83, 297}) {
            for (int iteration = 0; iteration < 3; ++iteration) {
                TUnversionedRowBuilder builder;

                std::vector<TUnversionedRow> bounds;
                for (int boundIndex = 0; boundIndex < rangeCount * 2; ++boundIndex) {
                    builder.Reset();
                    keyGenerator->GenerateBound(&builder, valueGenerator, &Rng_);
                    bounds.push_back(RowBuffer_->CaptureRow(builder.GetRow(), false));
                }

                std::sort(bounds.begin(), bounds.end());

                YT_VERIFY(bounds.size() % 2 == 0);

                std::vector<TRowRange> readRanges;
                for (int rangeIndex = 0; rangeIndex < std::ssize(bounds) / 2; ++rangeIndex) {
                    readRanges.push_back(TRowRange(bounds[2 * rangeIndex], bounds[2 * rangeIndex + 1]));
                }

                auto columnFilter = generateColumnFilter
                    ? GenerateColumnFilter(readSchema)
                    : TColumnFilter();

                TestRangeReader(
                    rows,
                    memoryReader,
                    WriteSchema_,
                    readSchema,
                    MakeSharedRange(readRanges, RowBuffer_),
                    columnFilter,
                    timestamp,
                    timestamp == AllCommittedTimestamp);
            }
        }
    }
};

TEST_P(TVersionedChunksStressTest, Test)
{
    DoStressTest();
}

const auto StressTestOptionsValues = testing::Values(
    TTestOptions{.OptimizeFor = EOptimizeFor::Scan},
    TTestOptions{.OptimizeFor = EOptimizeFor::Scan, .UseNewReader = true},
    TTestOptions{.OptimizeFor = EOptimizeFor::Lookup},
#if !defined(_asan_enabled_) && !defined(_msan_enabled_)
    TTestOptions{.OptimizeFor = EOptimizeFor::Lookup, .ChunkFormat = EChunkFormat::TableVersionedIndexed},
#endif
    TTestOptions{.OptimizeFor = EOptimizeFor::Lookup, .ChunkFormat = EChunkFormat::TableVersionedSlim});

INSTANTIATE_TEST_SUITE_P(
    TVersionedChunksStressTest,
    TVersionedChunksStressTest,
    ::testing::Combine(
        StressTestOptionsValues,
        ::testing::Bool()),
    [] (const auto& info) {
        return Format("%v%v",
            ToString(std::get<0>(info.param)),
            std::get<1>(info.param) ? "Groups" : "");
    });

// TODO(akozhikhov): More thorough test for aggregate columns.

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NTableClient
