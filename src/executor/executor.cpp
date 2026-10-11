#include <executor/executor.h>
#include <executor/operators/delete.h>
#include <executor/operators/filter.h>
#include <executor/operators/insert.h>
#include <executor/operators/project.h>
#include <executor/operators/scan.h>
#include <optimizer/physical_plan.h>

#include <stdexcept>
#include <string>

namespace AxolotlDB::executor {

namespace {

using optimizer::PhysicalNode;
using optimizer::PhysicalNodeType;
using ExecutorPtr = std::unique_ptr<AbstractExecutor>;

void ExpectChildren(const PhysicalNode& node, std::size_t expected, const char* name) {
    if (node.children.size() != expected) {
        throw std::logic_error(std::string("CreateExecutor: ") + name + " expects " +
                               std::to_string(expected) + " child(ren), plan has " +
                               std::to_string(node.children.size()));
    }
}

ExecutorPtr BuildChild(const PhysicalNode& node, std::size_t i, ExecutorContext* ctx) {
    return CreateExecutor(*node.children[i], ctx);
}

[[noreturn]] void NotImplemented(const char* name) {
    throw std::logic_error(std::string("CreateExecutor: ") + name +
                           " executor is not implemented yet");
}

}  // namespace

ExecutorPtr CreateExecutor(const PhysicalNode& plan, ExecutorContext* ctx) {
    if (ctx == nullptr) {
        throw std::invalid_argument("CreateExecutor: null context");
    }

    switch (plan.Type()) {
        case PhysicalNodeType::SeqScan: {
            ExpectChildren(plan, 0, "SeqScan");
            return std::make_unique<SeqScanExecutor>(
                ctx, static_cast<const optimizer::PhysicalSeqScan&>(plan));
        }
        case PhysicalNodeType::Insert: {
            ExpectChildren(plan, 0, "Insert");
            return std::make_unique<InsertExecutor>(
                ctx, static_cast<const optimizer::PhysicalInsert&>(plan));
        }
        case PhysicalNodeType::Filter: {
            ExpectChildren(plan, 1, "Filter");
            auto child = BuildChild(plan, 0, ctx);
            return std::make_unique<FilterExecutor>(
                ctx, static_cast<const optimizer::PhysicalFilter&>(plan), std::move(child));
        }
        case PhysicalNodeType::Project: {
            ExpectChildren(plan, 1, "Project");
            auto child = BuildChild(plan, 0, ctx);
            return std::make_unique<ProjectionExecutor>(
                ctx, static_cast<const optimizer::PhysicalProject&>(plan), std::move(child));
        }
        case PhysicalNodeType::HashAggregate: {
            ExpectChildren(plan, 1, "HashAggregate");
            auto child = BuildChild(plan, 0, ctx);
            (void)child;
            NotImplemented("HashAggregate");
        }
        case PhysicalNodeType::Sort: {
            ExpectChildren(plan, 1, "Sort");
            auto child = BuildChild(plan, 0, ctx);
            (void)child;
            NotImplemented("Sort");
        }
        case PhysicalNodeType::Limit: {
            ExpectChildren(plan, 1, "Limit");
            auto child = BuildChild(plan, 0, ctx);
            (void)child;
            NotImplemented("Limit");
        }
        case PhysicalNodeType::Update: {
            ExpectChildren(plan, 1, "Update");
            auto child = BuildChild(plan, 0, ctx);
            (void)child;
            NotImplemented("Update");
        }
        case PhysicalNodeType::Delete: {
            ExpectChildren(plan, 1, "Delete");
            auto child = BuildChild(plan, 0, ctx);
            (void)child;
            return std::make_unique<DeleteExecutor>(
                ctx, static_cast<const optimizer::PhysicalDelete&>(plan), std::move(child));
        }
        case PhysicalNodeType::NestedLoopJoin: {
            ExpectChildren(plan, 2, "NestedLoopJoin");
            auto left = BuildChild(plan, 0, ctx);
            auto right = BuildChild(plan, 1, ctx);
            (void)left;
            (void)right;
            NotImplemented("NestedLoopJoin");
        }
        case PhysicalNodeType::HashJoin: {
            ExpectChildren(plan, 2, "HashJoin");
            auto build = BuildChild(plan, 0, ctx);
            auto probe = BuildChild(plan, 1, ctx);
            (void)build;
            (void)probe;
            NotImplemented("HashJoin");
        }
        case PhysicalNodeType::CreateTable:
            NotImplemented("CreateTable");
        case PhysicalNodeType::DropTable:
            NotImplemented("DropTable");
    }

    throw std::logic_error("CreateExecutor: unknown physical node type");
}

}  // namespace AxolotlDB::executor