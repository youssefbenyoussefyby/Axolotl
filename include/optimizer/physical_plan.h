#pragma once

#include <optimizer/logical_plan.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace AxolotlDB::optimizer {
enum class PhysicalNodeType {
    SeqScan,
    Filter,
    Project,
    NestedLoopJoin,
    HashJoin,
    HashAggregate,
    Sort,
    Limit,
    Insert,
    Update,
    Delete,
    CreateTable,
    DropTable,
};

class PhysicalNode {
   public:
    virtual ~PhysicalNode() = default;

    [[nodiscard]] virtual PhysicalNodeType Type() const noexcept = 0;

    std::vector<OutputColumn> outputSchema;
    std::vector<std::unique_ptr<PhysicalNode>> children;
};

using PhysicalNodePtr = std::unique_ptr<PhysicalNode>;

struct PhysicalSeqScan : PhysicalNode {
    catalog::TableId tableId = 0;
    std::string tableName;
    std::optional<std::string> alias;
    storage::Schema schema;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::SeqScan;
    }
};

struct PhysicalFilter : PhysicalNode {
    parser::ast::BoundExprPtr predicate;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::Filter;
    }
};

struct PhysicalProject : PhysicalNode {
    std::vector<ProjectColumn> projections;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::Project;
    }
};

struct PhysicalJoin : PhysicalNode {
    parser::ast::JoinType joinType = parser::ast::JoinType::INNER;
    parser::ast::BoundExprPtr condition;
};

struct PhysicalNestedLoopJoin : PhysicalJoin {
    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::NestedLoopJoin;
    }
};

struct PhysicalHashJoin : PhysicalJoin {
    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::HashJoin;
    }
};

struct PhysicalHashAggregate : PhysicalNode {
    std::vector<parser::ast::BoundExprPtr> groupBy;
    std::vector<AggregateColumn> aggregates;
    parser::ast::BoundExprPtr having;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::HashAggregate;
    }
};

struct PhysicalSort : PhysicalNode {
    std::vector<SortKey> keys;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::Sort;
    }
};

struct PhysicalLimit : PhysicalNode {
    parser::ast::BoundExprPtr limit;
    parser::ast::BoundExprPtr offset;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::Limit;
    }
};

struct PhysicalInsert : PhysicalNode {
    catalog::TableId tableId = 0;
    std::string tableName;
    std::vector<uint32_t> columnIndices;
    std::vector<storage::ColumnDef> targetColumns;
    std::vector<InsertRow> rows;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::Insert;
    }
};

struct PhysicalUpdate : PhysicalNode {
    catalog::TableId tableId = 0;
    std::string tableName;
    std::vector<UpdateAssignment> assignments;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::Update;
    }
};

struct PhysicalDelete : PhysicalNode {
    catalog::TableId tableId = 0;
    std::string tableName;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::Delete;
    }
};

struct PhysicalCreateTable : PhysicalNode {
    std::string tableName;
    storage::Schema schema;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::CreateTable;
    }
};

struct PhysicalDropTable : PhysicalNode {
    catalog::TableId tableId = 0;
    std::string tableName;

    [[nodiscard]] PhysicalNodeType Type() const noexcept override {
        return PhysicalNodeType::DropTable;
    }
};

enum class JoinAlgorithm { NestedLoop, Hash };

[[nodiscard]] JoinAlgorithm SelectJoinAlgorithm(const LogicalNestedLoopJoin& join);

/// @throws std::invalid_argument on a null node,
///         std::logic_error on a logical type with no mapping.
[[nodiscard]] PhysicalNodePtr ToPhysicalPlan(LogicalNodePtr logical);

[[nodiscard]] std::string PlanToString(const PhysicalNode& root);
}  // namespace AxolotlDB::optimizer