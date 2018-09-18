#include "storage_table.h"

#include "auth_token.h"
#include "query_helpers.h"
#include "storage_distributed.h"
#include "virtual_columns.h"

#include <Common/Exception.h>
#include <Common/typeid_cast.h>
#include <Parsers/ASTFunction.h>
#include <Parsers/ASTLiteral.h>
#include <Parsers/ASTSelectQuery.h>
#include <Parsers/ASTTablesInSelectQuery.h>
#include <Parsers/queryToString.h>

#include <common/logger_useful.h>

namespace DB {

namespace ErrorCodes
{
    extern const int LOGICAL_ERROR;
}

}   // namespace DB

namespace NYT {
namespace NClickHouse {

using namespace DB;

////////////////////////////////////////////////////////////////////////////////

class TStorageTable final
    : public TStorageDistributed
{
private:
    NInterop::TTablePtr Table;

public:
    TStorageTable(
        NInterop::IStoragePtr storage,
        NInterop::TTablePtr table,
        IExecutionClusterPtr cluster);

    std::string getTableName() const override
    {
        return Table->Name;
    }

private:
    const DB::NamesAndTypesList& ListVirtualColumns() const override
    {
        return ListSystemVirtualColumns();
    }

    NInterop::TTablePartList GetTableParts(
        const ASTPtr& queryAst,
        const Context& context,
        NInterop::IRangeFilterPtr rangeFilter,
        size_t maxParts) override;

    ASTPtr RewriteSelectQueryForTablePart(
        const ASTPtr& queryAst,
        const std::string& jobSpec) override;
};

////////////////////////////////////////////////////////////////////////////////

TStorageTable::TStorageTable(NInterop::IStoragePtr storage,
                             NInterop::TTablePtr table,
                             IExecutionClusterPtr cluster)
    : TStorageDistributed(
        std::move(storage),
        std::move(cluster),
        TTableSchema::From(*table),
        &Poco::Logger::get("StorageTable"))
    , Table(std::move(table))
{}

NInterop::TTablePartList TStorageTable::GetTableParts(
    const ASTPtr& queryAst,
    const Context& context,
    NInterop::IRangeFilterPtr rangeFilter,
    size_t maxParts)
{
    Y_UNUSED(queryAst);

    auto& storage = GetStorage();

    auto authToken = CreateAuthToken(*storage, context);

    return storage->GetTableParts(
        *authToken,
        Table->Name,
        rangeFilter,
        maxParts);
}

ASTPtr TStorageTable::RewriteSelectQueryForTablePart(
    const ASTPtr& queryAst,
    const std::string& jobSpec)
{
    auto modifiedQueryAst = queryAst->clone();

    ASTPtr tableFunction;

    auto* tableExpression = GetFirstTableExpression(typeid_cast<ASTSelectQuery &>(*modifiedQueryAst));
    if (tableExpression) {
        if (tableExpression->table_function) {
            auto& function = typeid_cast<ASTFunction &>(*tableExpression->table_function);
            if (function.name == "ytTable") {
                // TODO: forward all args
                tableFunction = makeASTFunction(
                    "ytTableData",
                    std::make_shared<ASTLiteral>(jobSpec));
            }
        } else {
            tableFunction = makeASTFunction(
                "ytTableData",
                std::make_shared<ASTLiteral>(jobSpec));
        }
    }

    if (!tableFunction) {
        throw Exception("Invalid SelectQuery", queryToString(queryAst), ErrorCodes::LOGICAL_ERROR);
    }

    tableExpression->table_function = std::move(tableFunction);
    tableExpression->database_and_table_name = nullptr;
    tableExpression->subquery = nullptr;

    return modifiedQueryAst;
}

////////////////////////////////////////////////////////////////////////////////

DB::StoragePtr CreateStorageTable(
    NInterop::IStoragePtr storage,
    NInterop::TTablePtr table,
    IExecutionClusterPtr cluster)
{
    return std::make_shared<TStorageTable>(
        std::move(storage),
        std::move(table),
        std::move(cluster));
}

} // namespace NClickHouse
} // namespace NYT
