#pragma once

#include <catalog/catalog.h>
#include <parser/ast/boundstmt.h>
#include <storage/tuple.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AxolotlDB::optimizer {
enum class LogicalNodeType {
    Scan,
    Filter,
    Project,
    NestedLoopJoin,
    Aggregate,
    Sort,
    Limit,
    Insert,
    Update,
    Delete,
    CreateTable,
    DropTable,
};

struct OutputColumn {
    std::string name;
    storage::ColumnType type;
    catalog::TableId tableId = 0;
    uint32_t columnIndex = 0;
};

class LogicalNode {
   public:
    virtual ~LogicalNode() = default;

    [[nodiscard]] virtual LogicalNodeType Type() const noexcept = 0;
    [[nodiscard]] virtual std::vector<OutputColumn> OutputSchema() const = 0;

    std::vector<std::unique_ptr<LogicalNode>> children;
};

using LogicalNodePtr = std::unique_ptr<LogicalNode>;

struct LogicalScan : LogicalNode {
    catalog::TableId tableId = 0;
    std::string tableName;
    std::optional<std::string> alias;
    storage::Schema schema;

    LogicalScan(catalog::TableId id, std::string name, std::optional<std::string> a,
                storage::Schema s)
        : tableId(id), tableName(std::move(name)), alias(std::move(a)), schema(std::move(s)) {}

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::Scan;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        std::vector<OutputColumn> cols;
        for (uint32_t i = 0; i < static_cast<uint32_t>(schema.columns.size()); i++) {
            cols.push_back({schema.columns[i].name, schema.columns[i].type, tableId, i});
        }
        return cols;
    }
};

struct LogicalFilter : LogicalNode {
    parser::ast::BoundExprPtr predicate;

    explicit LogicalFilter(parser::ast::BoundExprPtr pred) : predicate(std::move(pred)) {}

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::Filter;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        return children.empty() ? std::vector<OutputColumn>{} : children[0]->OutputSchema();
    }
};

struct ProjectColumn {
    parser::ast::BoundExprPtr expr;
    std::string name;
};

struct LogicalProject : LogicalNode {
    std::vector<ProjectColumn> projections;

    explicit LogicalProject(std::vector<ProjectColumn> proj) : projections(std::move(proj)) {}

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::Project;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        std::vector<OutputColumn> cols;
        for (const auto& p : projections) {
            cols.push_back({p.name, p.expr->type, 0, 0});
        }
        return cols;
    }
};

struct LogicalNestedLoopJoin : LogicalNode {
    parser::ast::JoinType joinType = parser::ast::JoinType::INNER;
    parser::ast::BoundExprPtr condition;

    LogicalNestedLoopJoin(parser::ast::JoinType jt, parser::ast::BoundExprPtr cond)
        : joinType(jt), condition(std::move(cond)) {}

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::NestedLoopJoin;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        std::vector<OutputColumn> cols;
        for (const auto& child : children) {
            for (auto& c : child->OutputSchema()) {
                cols.push_back(c);
            }
        }
        return cols;
    }
};

struct AggregateColumn {
    parser::ast::BoundExprPtr expr;
    std::string name;
    bool isAggregate = false;
};

struct LogicalAggregate : LogicalNode {
    std::vector<parser::ast::BoundExprPtr> groupBy;
    std::vector<AggregateColumn> aggregates;
    parser::ast::BoundExprPtr having;

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::Aggregate;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        std::vector<OutputColumn> cols;
        for (const auto& a : aggregates) {
            cols.push_back({a.name, a.expr->type, 0, 0});
        }
        return cols;
    }
};

struct SortKey {
    parser::ast::BoundExprPtr expr;
    bool descending = false;
};

struct LogicalSort : LogicalNode {
    std::vector<SortKey> keys;

    explicit LogicalSort(std::vector<SortKey> k) : keys(std::move(k)) {}

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::Sort;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        return children.empty() ? std::vector<OutputColumn>{} : children[0]->OutputSchema();
    }
};

struct LogicalLimit : LogicalNode {
    parser::ast::BoundExprPtr limit;
    parser::ast::BoundExprPtr offset;

    LogicalLimit(parser::ast::BoundExprPtr l, parser::ast::BoundExprPtr o)
        : limit(std::move(l)), offset(std::move(o)) {}

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::Limit;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        return children.empty() ? std::vector<OutputColumn>{} : children[0]->OutputSchema();
    }
};

struct InsertRow {
    std::vector<parser::ast::BoundExprPtr> values;
};

struct LogicalInsert : LogicalNode {
    catalog::TableId tableId = 0;
    std::string tableName;
    std::vector<uint32_t> columnIndices;
    std::vector<storage::ColumnDef> targetColumns;
    std::vector<InsertRow> rows;

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::Insert;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        return {{"rows_affected", storage::ColumnType::INT64, 0, 0}};
    }
};

struct UpdateAssignment {
    uint32_t columnIndex = 0;
    parser::ast::BoundExprPtr value;
};

struct LogicalUpdate : LogicalNode {
    catalog::TableId tableId = 0;
    std::string tableName;
    std::vector<UpdateAssignment> assignments;

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::Update;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        return {{"rows_affected", storage::ColumnType::INT64, 0, 0}};
    }
};

struct LogicalDelete : LogicalNode {
    catalog::TableId tableId = 0;
    std::string tableName;

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::Delete;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        return {{"rows_affected", storage::ColumnType::INT64, 0, 0}};
    }
};

struct LogicalCreateTable : LogicalNode {
    std::string tableName;
    storage::Schema schema;

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::CreateTable;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        return {};
    }
};

struct LogicalDropTable : LogicalNode {
    catalog::TableId tableId = 0;
    std::string tableName;

    [[nodiscard]] LogicalNodeType Type() const noexcept override {
        return LogicalNodeType::DropTable;
    }

    [[nodiscard]] std::vector<OutputColumn> OutputSchema() const override {
        return {};
    }
};
}  // namespace AxolotlDB::optimizer
