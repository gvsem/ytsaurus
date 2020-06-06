#include "wire_row_stream.h"
#include "row_stream.h"
#include "helpers.h"

#include <yt/client/table_client/unversioned_row.h>
#include <yt/client/table_client/unversioned_row_batch.h>
#include <yt/client/table_client/name_table.h>
#include <yt/client/table_client/wire_protocol.h>
#include <yt/client/table_client/row_buffer.h>

#include <yt/core/misc/range.h>
#include <yt/core/misc/protobuf_helpers.h>

namespace NYT::NApi::NRpcProxy {

using namespace NTableClient;

////////////////////////////////////////////////////////////////////////////////

class TWireRowStreamFormatter
    : public IRowStreamFormatter
{
public:
    explicit TWireRowStreamFormatter(TNameTablePtr nameTable)
        : NameTable_(std::move(nameTable))
    { }
    
    virtual TSharedRef Format(
        const IUnversionedRowBatchPtr& batch,
        const NApi::NRpcProxy::NProto::TRowsetStatistics* statistics) override
    {
        YT_VERIFY(NameTableSize_ <= NameTable_->GetSize());

        NProto::TRowsetDescriptor descriptor;
        descriptor.set_wire_format_version(NApi::NRpcProxy::CurrentWireFormatVersion);
        descriptor.set_rowset_kind(NProto::RK_UNVERSIONED);
        for (int id = NameTableSize_; id < NameTable_->GetSize(); ++id) {
            auto* entry = descriptor.add_name_table_entries();
            entry->set_name(TString(NameTable_->GetName(id)));
        }
        NameTableSize_ += descriptor.name_table_entries_size();
        
        TWireProtocolWriter writer;
        auto rows = batch->MaterializeRows();
        writer.WriteUnversionedRowset(rows);
        auto rowRefs = writer.Finish();

        auto descriptorRef = SerializeProtoToRef(descriptor);
        
        struct TWireRowStreamFormatterTag { };
        auto mergedRowRefs = MergeRefsToRef<TWireRowStreamFormatterTag>(rowRefs);
        
        auto rowsetData = PackRefs(std::vector{descriptorRef, mergedRowRefs});;

        if (statistics) {
            auto statisticsRef = SerializeProtoToRef(*statistics);
            return PackRefs(std::vector{rowsetData, statisticsRef});
        } else {
            return rowsetData;
        }
    }

private:
    const TNameTablePtr NameTable_;

    int NameTableSize_ = 0;
};

IRowStreamFormatterPtr CreateWireRowStreamFormatter(TNameTablePtr nameTable)
{
    return New<TWireRowStreamFormatter>(std::move(nameTable));
}

////////////////////////////////////////////////////////////////////////////////

class TWireRowStreamParser
    : public IRowStreamParser
{
public:
    explicit TWireRowStreamParser(TNameTablePtr nameTable)
        : NameTable_(std::move(nameTable))
    {
        Descriptor_.set_wire_format_version(NApi::NRpcProxy::CurrentWireFormatVersion);
        Descriptor_.set_rowset_kind(NApi::NRpcProxy::NProto::RK_UNVERSIONED);
    }

    virtual TSharedRange<TUnversionedRow> Parse(
        const TSharedRef& block,
        NProto::TRowsetStatistics* statistics) override
    {
        TSharedRef rowsRef;
        if (statistics) {
            auto parts = UnpackRefsOrThrow(block);
            if (parts.size() != 2) {
                THROW_ERROR_EXCEPTION(
                    "Error deserializing rows: expected %v packed refs, got %v",
                    2,
                    parts.size());
            }

            rowsRef = parts[0];

            const auto& statisticsRef = parts[1];
            if (!TryDeserializeProto(statistics, statisticsRef)) {
                THROW_ERROR_EXCEPTION("Error deserializing rowset statistics");
            }
        } else {
            rowsRef = block;
        }

        auto parts = UnpackRefsOrThrow(rowsRef);
        if (parts.size() != 2) {
            THROW_ERROR_EXCEPTION(
                "Error deserializing rowset with name table delta: expected %v packed refs, got %v",
                2,
                parts.size());
        }

        const auto& descriptorDeltaRef = parts[0];
        const auto& mergedRowRefs = parts[1];

        NApi::NRpcProxy::NProto::TRowsetDescriptor descriptorDelta;
        if (!TryDeserializeProto(&descriptorDelta, descriptorDeltaRef)) {
            THROW_ERROR_EXCEPTION("Error deserializing rowset descriptor delta");
        }
        NApi::NRpcProxy::ValidateRowsetDescriptor(
            descriptorDelta,
            NApi::NRpcProxy::CurrentWireFormatVersion,
            NApi::NRpcProxy::NProto::RK_UNVERSIONED);

        struct TWireRowStreamParserTag { };
        TWireProtocolReader reader(mergedRowRefs, New<TRowBuffer>(TWireRowStreamParserTag()));
        auto rows = reader.ReadUnversionedRowset(true);

        auto oldNameTableSize = Descriptor_.name_table_entries_size();
        YT_VERIFY(oldNameTableSize <= NameTable_->GetSize());
        
        Descriptor_.MergeFrom(descriptorDelta);
        auto newNameTableSize = Descriptor_.name_table_entries_size();
        
        IdMapping_.resize(newNameTableSize);
        for (int id = oldNameTableSize; id < newNameTableSize; ++id) {
            const auto& name = Descriptor_.name_table_entries(id).name();
            auto mappedId = NameTable_->GetIdOrRegisterName(name);
            IdMapping_[id] = mappedId;
            HasNontrivialIdMapping_ |= (id != mappedId);
        }

        if (HasNontrivialIdMapping_) {
            for (auto row : rows) {
                auto mutableRow = TMutableUnversionedRow(row.ToTypeErasedRow());
                for (auto& value : mutableRow) {
                    auto newId = ApplyIdMapping(value, &IdMapping_);
                    if (newId < 0 || newId >= NameTable_->GetSize()) {
                        THROW_ERROR_EXCEPTION("Id mapping returned an invalid value %v for id %v: "
                            "expected a value in [0, %v) range",
                            newId,
                            value.Id,
                            NameTable_->GetSize());
                    }
                    value.Id = newId;
                }
            }
        }

        return rows;
    }

private:
    const TNameTablePtr NameTable_;

    NApi::NRpcProxy::NProto::TRowsetDescriptor Descriptor_;
    TNameTableToSchemaIdMapping IdMapping_;
    bool HasNontrivialIdMapping_ = false;
};

IRowStreamParserPtr CreateWireRowStreamParser(TNameTablePtr nameTable)
{
    return New<TWireRowStreamParser>(std::move(nameTable));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NApi::NRpcProxy

