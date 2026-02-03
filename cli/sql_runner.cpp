// Engine glue for the CLI.
//
// Adapted to the real headers in include/ and src/.
// Every previous "ADAPT" site has been resolved against the actual API.

#include "sql_runner.h"

#include <chrono>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

// Engine headers — paths follow the layout in include/.
#include <catalog/catalog.h>
#include <executor/executor.h>
#include <executor/executor_context.h>
#include <optimizer/logical_plan.h>
#include <optimizer/physical_plan.h>
#include <optimizer/planner.h>
#include <parser/binder.h>
#include <parser/parser.h>
#include <storage/storage_manager.h>
#include <storage/tuple.h>
#include <utils/diagnosticsink.h>
#include <utils/errors.h>
#include<parser/ast/printer.h>

namespace fs = std::filesystem;

// Pull in only what we need from each sub-namespace to keep code readable.
using AxolotlDB::storage::ColumnDef;
using AxolotlDB::storage::ColumnType;
using AxolotlDB::storage::Schema;
using AxolotlDB::storage::StorageManager;
using AxolotlDB::storage::TableHeap;
using AxolotlDB::storage::Tuple;
using AxolotlDB::storage::Value;
using AxolotlDB::catalog::Catalog;
using AxolotlDB::catalog::TableId;
using AxolotlDB::executor::ExecutorContext;
using AxolotlDB::executor::AbstractExecutor;
using AxolotlDB::executor::CreateExecutor;
using AxolotlDB::optimizer::Planner;
using AxolotlDB::optimizer::ToPhysicalPlan;
using AxolotlDB::parser::Parser;
using AxolotlDB::binder::Binder;
using AxolotlDB::utils::DiagnosticSink;
using AxolotlDB::utils::SyntaxException;
using AxolotlDB::parser::ast::ToString;

namespace {

// Render a single Value as a printable string.
std::string Render(const Value& v) {
    if (v.IsNull()) {
        return "NULL";
    }
    switch (v.Type()) {
        case ColumnType::INT32:
            return std::to_string(v.AsInt32());
        case ColumnType::INT64:
            return std::to_string(v.AsInt64());
        case ColumnType::FLOAT32: {
            std::ostringstream os;
            os << v.AsFloat32();
            return os.str();
        }
        case ColumnType::FLOAT64: {
            std::ostringstream os;
            os << v.AsFloat64();
            return os.str();
        }
        case ColumnType::BOOLEAN:
            return v.AsBool() ? "true" : "false";
        case ColumnType::VARCHAR:
            return v.AsVarchar();
        case ColumnType::VECTOR:
            return "<vector>";
    }
    return "?";
}

}  // namespace

// ─── Impl ────────────────────────────────────────────────────────────────────

struct SqlRunner::Impl {
    // StorageManager must outlive Catalog.
    explicit Impl() : sm(), catalog(sm) {}

    ~Impl() {
        try {
            sm.FlushAll();
        } catch (...) {
        }
    }

    StorageManager sm;
    Catalog        catalog;
};

// ─── SqlRunner ───────────────────────────────────────────────────────────────

SqlRunner::SqlRunner(const fs::path& data_dir) {
    fs::create_directories(data_dir);
    // StorageManager / Catalog open files relative to the working directory,
    // so we cd into the data directory before constructing Impl.
    fs::current_path(data_dir);
    impl_ = std::make_unique<Impl>();
}

SqlRunner::~SqlRunner() = default;

QueryResult SqlRunner::Execute(const std::string& sql) {
    QueryResult result;
    const auto t0 = std::chrono::steady_clock::now();

    try {
        DiagnosticSink diag;

        // 1. Text → AST
        Parser parser(sql, diag);
        auto statement = parser.Parse();
        std::cout <<  ToString(statement) << '\n';
        // 2. AST → names/types resolved against the catalog
        Binder binder(impl_->catalog);
        auto bound_ptr = binder.Bind(statement);   // returns unique_ptr<BoundStatement>

        // 3. Bound → logical plan
        Planner planner;
        auto logical = planner.Plan(*bound_ptr);   // Plan takes const BoundStatement&

        // 4. Logical → physical plan
        auto physical = ToPhysicalPlan(std::move(logical));

        // 5. Physical → executor tree, then pull rows in batches
        ExecutorContext ctx(impl_->sm, impl_->catalog, /*transaction=*/nullptr);
        auto executor = CreateExecutor(*physical, &ctx);
        executor->Init();

        // Collect output column names from the physical plan's output schema.
        const Schema& schema = executor->OutputSchema();
        for (const auto& col : schema.columns) {
            result.columns.push_back(col.name);
        }

        // Pull rows batch by batch.
        std::vector<AxolotlDB::storage::Tuple> batch;
        std::optional<std::vector<AxolotlDB::storage::RID>> rid_batch;

        while (executor->Next(&batch, rid_batch)) {
            for (const auto& tuple : batch) {
                auto values = Tuple::Deserialize(schema, tuple.Span());
                std::vector<std::string> row;
                row.reserve(values.size());
                for (const auto& v : values) {
                    row.push_back(Render(v));
                }
                result.rows.push_back(std::move(row));
            }
        }

    } catch (const SyntaxException& e) {
        result.error = SqlError{"syntax error", e.what(),
                                static_cast<int>(e.line),
                                static_cast<int>(e.column)};
    } catch (const std::exception& e) {
        result.error = SqlError{"error", e.what(), 0, 0};
    }

    result.elapsed_ms =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0)
            .count();
    return result;
}

// ─── SeedDemo ────────────────────────────────────────────────────────────────

std::string SqlRunner::SeedDemo() {
    // If the table already exists, nothing to do.
    try {
        (void)impl_->catalog.GetTableId("king_ya_bay");
        return "table `king_ya_bay` already exists, nothing to do";
    } catch (const std::exception&) {
        // not there yet: create it below
    }

    Schema schema({
        ColumnDef("id",    ColumnType::INT32),
        ColumnDef("name",  ColumnType::VARCHAR),
        ColumnDef("age",   ColumnType::INT32),
        ColumnDef("aura", ColumnType::FLOAT64),
    });

    const TableId table = impl_->catalog.CreateTable("king_ya_bay", schema);
    TableHeap& heap = impl_->catalog.GetTableHeap(table);

    struct Row {
        int32_t     id;
        const char* name;
        int32_t     age;
        double      score;
    };
    const Row rows[] = {
        {1,  "Youssef Ben Youssef",  23, 9999999}, {2,  "Raed Aroua",  23, 99999}, {3,  "Mouahanned Mejri",  22, 0},
        {4,  "Mouahamed Hedda",   23, 9999}, {5,  "ALi Backouchi",  23, 999},{6,"Ala Grein",9} 
    };

    for (const Row& r : rows) {
        std::vector<Value> vals = {
            Value::Int32(r.id),
            Value::Varchar(std::string(r.name)),
            Value::Int32(r.age),
            Value::Float64(r.score),
        };
        // Serialize to bytes, then insert into the heap.
        auto bytes = Tuple::Serialize(schema, vals);
        (void)heap.Insert(bytes);
    }

    impl_->sm.FlushAll();
    return "created table `king_ya_bay` (id, name, age, aura) with 6 rows";
}

// ─── TableNames ──────────────────────────────────────────────────────────────

std::vector<std::string> SqlRunner::TableNames() {
    // Catalog::GetSchema(name) throws when the name is not registered; we
    // instead walk name_to_id_ through the public API by trying every name we
    // know. Since Catalog exposes no list API, we ask for a table by id
    // starting from 1 and stop on the first exception.
    //
    // A cleaner approach: call GetTableName(id) for id = 1..N and stop on
    // exception; that avoids needing to know names up front.
    std::vector<std::string> names;
    for (uint32_t id = 1; ; ++id) {
        try {
            names.push_back(impl_->catalog.GetTableName(id));
        } catch (const std::exception&) {
            break;
        }
    }
    return names;
}