#include <catalog/catalog.h>
#include <executor/executor.h>
#include <executor/executor_context.h>
#include <gtest/gtest.h>
#include <optimizer/physical_plan.h>
#include <optimizer/planner.h>
#include <parser/binder.h>
#include <parser/parser.h>
#include <storage/storage_manager.h>
#include <storage/table_heap.h>
#include <storage/tuple.h>
#include <utils/diagnosticsink.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace AxolotlDB;
using storage::ColumnDef;
using storage::ColumnType;
using storage::Schema;
using storage::Tuple;
using storage::Value;

class ExecutorMilestone : public ::testing::Test {
   protected:
    std::filesystem::path dir_;
    std::filesystem::path orig_dir_;

    void SetUp() override {
        orig_dir_ = std::filesystem::current_path();
        dir_ = std::filesystem::temp_directory_path() /
               ("axo_exec_milestone_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        std::filesystem::remove_all(dir_);
        std::filesystem::create_directories(dir_);
        std::filesystem::current_path(dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::current_path(orig_dir_, ec);
        std::filesystem::remove_all(dir_, ec);
    }
};

TEST_F(ExecutorMilestone, SelectWithWhereReturnsCorrectRowsFromDisk) {
    Schema userSchema{{ColumnDef{"id", ColumnType::INT32}, ColumnDef{"name", ColumnType::VARCHAR},
                       ColumnDef{"age", ColumnType::INT32}}};

    {
        storage::StorageManager sm(2);
        catalog::Catalog cat(sm);

        cat.CreateTable("users", userSchema);
        auto& heap = cat.GetTableHeap("users");

        for (int i = 1; i <= 60; ++i) {
            std::string namePadding(300, static_cast<char>('a' + (i % 26)));
            std::string name = "user_" + std::to_string(i) + "_" + namePadding;
            auto bytes = Tuple::Serialize(userSchema,
                                          {Value::Int32(i), Value::Varchar(name), Value::Int32(i)});
            (void)heap.Insert(bytes);
        }
        sm.FlushAll();
    }

    {
        storage::StorageManager sm(2);
        catalog::Catalog cat(sm);
        executor::ExecutorContext ctx(sm, cat, nullptr, 4);

        utils::DiagnosticSink sink;
        parser::Parser p("SELECT id, age FROM users WHERE age > 50;", sink);
        auto stmt = p.Parse();

        binder::Binder b(cat, &sink);
        auto bound = b.Bind(stmt);
        ASSERT_NE(bound, nullptr);

        optimizer::Planner planner;
        auto logical = planner.Plan(*bound);
        ASSERT_NE(logical, nullptr);

        auto physical = optimizer::ToPhysicalPlan(std::move(logical));
        ASSERT_NE(physical, nullptr);

        auto exec = executor::CreateExecutor(*physical, &ctx);
        ASSERT_NE(exec, nullptr);

        exec->Init();

        std::vector<Tuple> batch;
        std::optional<std::vector<storage::RID>> rids = std::nullopt;
        std::vector<int32_t> matchedIds;

        while (exec->Next(&batch, rids)) {
            const auto& outSchema = exec->GetOutputSchema();
            for (const auto& row : batch) {
                auto vals = Tuple::Deserialize(outSchema, row.Span());
                matchedIds.push_back(vals[0].AsInt32());
            }
        }

        ASSERT_EQ(matchedIds.size(), 10);
        for (int i = 0; i < 10; ++i) {
            EXPECT_EQ(matchedIds[i], 51 + i);
        }
    }
}
