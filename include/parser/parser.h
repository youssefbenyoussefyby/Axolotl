#pragma once

#include <parser/ast.h>
#include <parser/lexer.h>
#include <utils/diagnosticsink.h>

#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace AxolotlDB::parser {
class Parser {
   private:
    parser::ast::Statement stmt_;
    size_t current_ = 0;
    std::vector<parser::Token> tokens_;
    utils::DiagnosticSink& diagnosticSink_;

    bool isEnd();
    bool check(parser::TokenKind t);
    [[nodiscard]] const parser::Token& Peek() const;
    [[nodiscard]] const parser::Token& Previous() const;
    bool Match(parser::TokenKind t);
    parser::Token Expect(parser::TokenKind t, const char* what);
    parser::Token Advance();

    parser::ast::ExprPtr ParseExpression(int minPrecedence = 0);
    parser::ast::ExprPtr ParsePrimary();

    parser::ast::SelectStatement ParseSelectStatement();
    std::vector<parser::ast::SelectItem> ParseProjectionList();
    std::optional<parser::ast::TableRef> ParseFrom();
    std::vector<parser::ast::JoinClause> ParseJoins();
    parser::ast::ExprPtr ParseWhere();
    std::vector<parser::ast::ExprPtr> ParseGroupBy();
    parser::ast::ExprPtr ParseHaving();
    std::vector<parser::ast::OrderByItem> ParseOrderBy();
    parser::ast::ExprPtr ParseLimit();
    parser::ast::ExprPtr ParseOffset();
    parser::ast::SelectItem ExpectSelectItem();
    parser::ast::TableRef ParseTablePrimary();
    parser::ast::OrderByItem ParseOrderByItem();
    std::string ExpectIdentifier();

    parser::ast::InsertStatement ParseInsertStatement();
    std::optional<std::vector<std::string>> ParseColumnsList();
    std::vector<std::vector<parser::ast::ExprPtr>> ParseValueTuple();
    parser::ast::TableName ParseTableName();

    parser::ast::UpdateStatement ParseUpdateStatement();
    std::vector<std::pair<std::string, parser::ast::ExprPtr>> ParseValuesAssignments();

    parser::ast::DeleteStatement ParseDeleteStatement();
    parser::ast::DropTableStatement ParseDrop();
    parser::ast::CreateTableStatement ParseCreateTable();
    std::vector<parser::ast::ColumnDef> ParseColumnsCreation();
    parser::ast::ColumnDef ParseColumnDef();

    void Error(const std::string& message);

   public:
    Parser(std::string_view sql, utils::DiagnosticSink& d);
    ~Parser();

    void Synchronize();
    parser::ast::Statement Parse();
    std::vector<parser::ast::Statement> ParseStatements();
    static std::unique_ptr<parser::ast::Statement> Parse(std::string_view sql,
                                                         utils::DiagnosticSink& d);
    static std::vector<parser::ast::Statement> ParseStatements(std::string_view sql,
                                                               utils::DiagnosticSink& d);
};

}  // namespace AxolotlDB::parser
