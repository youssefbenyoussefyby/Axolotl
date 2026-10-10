#include <parser/parser.h>
#include <utils/errors.h>

namespace AxolotlDB::parser {
Parser::Parser(std::string_view sql, utils::DiagnosticSink& diagnosticSink)
    : diagnosticSink_(diagnosticSink) {
    std::string sqlString(sql);
    Lexer lexer(sqlString, diagnosticSink);
    tokens_ = lexer.ScanTokens();
    current_ = 0;
}

Parser::~Parser() = default;

const parser::Token& Parser::Peek() const {
    if (current_ >= tokens_.size()) {
        return tokens_.back();
    }
    return tokens_[current_];
}

const parser::Token& Parser::Previous() const {
    if (current_ == 0) {
        return tokens_.front();
    }
    return tokens_[current_ - 1];
}

bool Parser::isEnd() {
    return Peek().GetKind() == parser::TokenKind::END_OF_FILE;
}

bool Parser::check(parser::TokenKind t) {
    if (isEnd())
        return false;
    return Peek().GetKind() == t;
}

bool Parser::Match(parser::TokenKind t) {
    if (check(t)) {
        Advance();
        return true;
    }
    return false;
}

parser::Token Parser::Advance() {
    if (!isEnd()) {
        current_++;
    }
    return Previous();
}

parser::Token Parser::Expect(parser::TokenKind t, const char* what) {
    if (check(t)) {
        return Advance();
    }
    const auto& tok = Peek();
    std::string msg = "Expected " + std::string(what) + " but got " + tok.GetLexem();
    Error(msg);
    return tok;
}

void Parser::Error(const std::string& message) {
    const auto& tok = Peek();
    diagnosticSink_.addDiagnostic(
        utils::Diagnostic{message, tok.GetLine(), tok.GetColumn(), tok.GetLexem()});
    throw utils::SyntaxException(message, tok.GetLine(), tok.GetColumn(), tok.GetLexem());
}

std::string Parser::ExpectIdentifier() {
    if (check(parser::TokenKind::IDENTIFIER) || check(parser::TokenKind::QUOTED_IDENTIFIER)) {
        return Advance().GetLexem();
    }
    const auto& tok = Peek();
    Error("Expected identifier but got " + tok.GetLexem());
    return "";
}

parser::ast::TableName Parser::ParseTableName() {
    const auto& tok = Peek();
    std::string name = ExpectIdentifier();
    return parser::ast::TableName(name, tok.GetLine(), tok.GetColumn());
}

parser::ast::TableRef Parser::ParseTablePrimary() {
    parser::ast::TableName tbl = ParseTableName();
    std::optional<std::string> alias;
    if (Match(parser::TokenKind::AS)) {
        alias = ExpectIdentifier();
    } else if (check(parser::TokenKind::IDENTIFIER) ||
               check(parser::TokenKind::QUOTED_IDENTIFIER)) {
        alias = Advance().GetLexem();
    }
    return parser::ast::TableRef(std::move(tbl), alias);
}

void Parser::Synchronize() {
    Advance();
    while (!isEnd()) {
        if (Previous().GetKind() == parser::TokenKind::SEMICOLON) {
            return;
        }
        switch (Peek().GetKind()) {
            case parser::TokenKind::SELECT:
            case parser::TokenKind::INSERT:
            case parser::TokenKind::UPDATE:
            case parser::TokenKind::DELETE:
            case parser::TokenKind::CREATE:
            case parser::TokenKind::DROP:
                return;
            default:
                Advance();
                break;
        }
    }
}

parser::ast::Statement Parser::Parse() {
    while (Match(parser::TokenKind::SEMICOLON)) {
    }

    const auto& startTok = Peek();
    uint16_t line = startTok.GetLine();
    uint16_t col = startTok.GetColumn();

    parser::ast::Statement result;

    if (Match(parser::TokenKind::SELECT)) {
        result = parser::ast::Statement(ParseSelectStatement(), line, col);
    } else if (Match(parser::TokenKind::INSERT)) {
        result = parser::ast::Statement(ParseInsertStatement(), line, col);
    } else if (Match(parser::TokenKind::UPDATE)) {
        result = parser::ast::Statement(ParseUpdateStatement(), line, col);
    } else if (Match(parser::TokenKind::DELETE)) {
        result = parser::ast::Statement(ParseDeleteStatement(), line, col);
    } else if (Match(parser::TokenKind::CREATE)) {
        result = parser::ast::Statement(ParseCreateTable(), line, col);
    } else if (Match(parser::TokenKind::DROP)) {
        result = parser::ast::Statement(ParseDrop(), line, col);
    } else {
        Error("Unexpected statement token: " + startTok.GetLexem());
    }

    Match(parser::TokenKind::SEMICOLON);
    return result;
}

std::vector<parser::ast::Statement> Parser::ParseStatements() {
    std::vector<parser::ast::Statement> statements;
    while (!isEnd() && Peek().GetKind() != parser::TokenKind::END_OF_FILE) {
        while (Match(parser::TokenKind::SEMICOLON)) {
        }
        if (isEnd() || Peek().GetKind() == parser::TokenKind::END_OF_FILE) {
            break;
        }
        try {
            statements.push_back(Parse());
        } catch (const utils::SyntaxException&) {
            Synchronize();
        }
    }
    return statements;
}

std::unique_ptr<parser::ast::Statement> Parser::Parse(std::string_view sql,
                                                      utils::DiagnosticSink& d) {
    Parser p(sql, d);
    return std::make_unique<parser::ast::Statement>(p.Parse());
}

std::vector<parser::ast::Statement> Parser::ParseStatements(std::string_view sql,
                                                            utils::DiagnosticSink& d) {
    Parser p(sql, d);
    return p.ParseStatements();
}

parser::ast::SelectStatement Parser::ParseSelectStatement() {
    parser::ast::SelectStatement stmt;
    if (Match(parser::TokenKind::DISTINCT))
        stmt.isDistinct = true;
    else
        Match(parser::TokenKind::ALL);

    stmt.projection = ParseProjectionList();
    stmt.fromTable = ParseFrom();
    stmt.joins = ParseJoins();

    if (Match(parser::TokenKind::WHERE)) {
        stmt.where = ParseExpression();
    }

    if (Match(parser::TokenKind::GROUP)) {
        Expect(parser::TokenKind::BY, "BY");
        do {
            stmt.groupBy.push_back(ParseExpression());
        } while (Match(parser::TokenKind::COMMA));
    }

    if (Match(parser::TokenKind::HAVING)) {
        stmt.having = ParseExpression();
    }

    if (Match(parser::TokenKind::ORDER)) {
        Expect(parser::TokenKind::BY, "BY");
        do {
            stmt.orderBy.push_back(ParseOrderByItem());
        } while (Match(parser::TokenKind::COMMA));
    }

    if (Match(parser::TokenKind::LIMIT)) {
        stmt.limit = ParseExpression();
    }

    if (Match(parser::TokenKind::OFFSET)) {
        stmt.offset = ParseExpression();
    }

    if (check(parser::TokenKind::WHERE) || check(parser::TokenKind::GROUP) ||
        check(parser::TokenKind::HAVING) || check(parser::TokenKind::ORDER) ||
        check(parser::TokenKind::LIMIT)) {
        Error("Clauses out of order in SELECT");
    }

    return stmt;
}

std::vector<parser::ast::SelectItem> Parser::ParseProjectionList() {
    std::vector<parser::ast::SelectItem> ans;
    do {
        ans.push_back(ExpectSelectItem());
    } while (Match(parser::TokenKind::COMMA));
    return ans;
}

parser::ast::SelectItem Parser::ExpectSelectItem() {
    const auto& tok = Peek();
    if (Match(parser::TokenKind::STAR)) {
        return parser::ast::SelectItem(std::make_unique<parser::ast::Expr>(
                                           parser::ast::Star{}, tok.GetLine(), tok.GetColumn()),
                                       "*");
    }

    if ((check(parser::TokenKind::IDENTIFIER) || check(parser::TokenKind::QUOTED_IDENTIFIER)) &&
        current_ + 2 < tokens_.size() &&
        tokens_[current_ + 1].GetKind() == parser::TokenKind::DOT &&
        tokens_[current_ + 2].GetKind() == parser::TokenKind::STAR) {
        std::string tbl = Advance().GetLexem();
        Advance();
        Advance();
        return parser::ast::SelectItem(std::make_unique<parser::ast::Expr>(
            parser::ast::Star{tbl}, tok.GetLine(), tok.GetColumn()));
    }

    parser::ast::ExprPtr expr = ParseExpression();
    std::optional<std::string> alias;
    if (Match(parser::TokenKind::AS)) {
        alias = ExpectIdentifier();
    } else if (check(parser::TokenKind::IDENTIFIER) ||
               check(parser::TokenKind::QUOTED_IDENTIFIER)) {
        alias = Advance().GetLexem();
    }
    return parser::ast::SelectItem(std::move(expr), alias);
}

std::optional<parser::ast::TableRef> Parser::ParseFrom() {
    if (Match(parser::TokenKind::FROM)) {
        return ParseTablePrimary();
    }
    Expect(parser::TokenKind::FROM, "FROM");
    return std::nullopt;
}

std::vector<parser::ast::JoinClause> Parser::ParseJoins() {
    std::vector<parser::ast::JoinClause> ans;
    while (check(parser::TokenKind::JOIN) || check(parser::TokenKind::CROSS) ||
           check(parser::TokenKind::LEFT) || check(parser::TokenKind::RIGHT) ||
           check(parser::TokenKind::FULL) || check(parser::TokenKind::INNER) ||
           check(parser::TokenKind::NATURAL)) {
        bool isNatural = Match(parser::TokenKind::NATURAL);
        parser::ast::JoinType jt = parser::ast::JoinType::INNER;
        if (Match(parser::TokenKind::LEFT)) {
            Match(parser::TokenKind::OUTER);
            jt = parser::ast::JoinType::LEFT;
        } else if (Match(parser::TokenKind::RIGHT)) {
            Match(parser::TokenKind::OUTER);
            jt = parser::ast::JoinType::RIGHT;
        } else if (Match(parser::TokenKind::FULL)) {
            Match(parser::TokenKind::OUTER);
            jt = parser::ast::JoinType::FULL;
        } else if (Match(parser::TokenKind::CROSS)) {
            jt = parser::ast::JoinType::CROSS;
        } else {
            Match(parser::TokenKind::INNER);
        }

        Expect(parser::TokenKind::JOIN, "JOIN");
        parser::ast::TableRef tbl = ParseTablePrimary();
        parser::ast::ExprPtr onCond;
        if (Match(parser::TokenKind::ON)) {
            onCond = ParseExpression();
        }
        std::vector<std::string> usingCols;
        if (Match(parser::TokenKind::USING)) {
            Expect(parser::TokenKind::LEFT_PAREN, "(");
            do {
                usingCols.push_back(ExpectIdentifier());
            } while (Match(parser::TokenKind::COMMA));
            Expect(parser::TokenKind::RIGHT_PAREN, ")");
        }
        parser::ast::JoinClause jc;
        jc.type = jt;
        jc.natural = isNatural;
        jc.table = std::move(tbl);
        jc.onCondition = std::move(onCond);
        jc.usingColumns = std::move(usingCols);
        ans.push_back(std::move(jc));
    }
    return ans;
}

parser::ast::ExprPtr Parser::ParseWhere() {
    if (Match(parser::TokenKind::WHERE)) {
        return ParseExpression();
    }
    return nullptr;
}

std::vector<parser::ast::ExprPtr> Parser::ParseGroupBy() {
    std::vector<parser::ast::ExprPtr> ans;
    if (Match(parser::TokenKind::GROUP)) {
        Expect(parser::TokenKind::BY, "BY");
        do {
            ans.push_back(ParseExpression());
        } while (Match(parser::TokenKind::COMMA));
    }
    return ans;
}

parser::ast::ExprPtr Parser::ParseHaving() {
    if (Match(parser::TokenKind::HAVING)) {
        return ParseExpression();
    }
    return nullptr;
}

std::vector<parser::ast::OrderByItem> Parser::ParseOrderBy() {
    std::vector<parser::ast::OrderByItem> ans;
    if (Match(parser::TokenKind::ORDER)) {
        Expect(parser::TokenKind::BY, "BY");
        do {
            ans.push_back(ParseOrderByItem());
        } while (Match(parser::TokenKind::COMMA));
    }
    return ans;
}

parser::ast::OrderByItem Parser::ParseOrderByItem() {
    parser::ast::ExprPtr expr = ParseExpression();
    bool isDesc = false;
    if (Match(parser::TokenKind::DESC)) {
        isDesc = true;
    } else {
        Match(parser::TokenKind::ASC);
    }
    return parser::ast::OrderByItem(std::move(expr), isDesc);
}

parser::ast::ExprPtr Parser::ParseLimit() {
    if (Match(parser::TokenKind::LIMIT)) {
        return ParseExpression();
    }
    return nullptr;
}

parser::ast::ExprPtr Parser::ParseOffset() {
    if (Match(parser::TokenKind::OFFSET)) {
        return ParseExpression();
    }
    return nullptr;
}

parser::ast::InsertStatement Parser::ParseInsertStatement() {
    Expect(parser::TokenKind::INTO, "INTO");
    parser::ast::InsertStatement stmt;
    stmt.table = ParseTableName();
    stmt.columns = ParseColumnsList();
    stmt.valueRows = ParseValueTuple();
    return stmt;
}

std::optional<std::vector<std::string>> Parser::ParseColumnsList() {
    if (Match(parser::TokenKind::LEFT_PAREN)) {
        std::vector<std::string> ans;
        do {
            ans.push_back(ExpectIdentifier());
        } while (Match(parser::TokenKind::COMMA));
        Expect(parser::TokenKind::RIGHT_PAREN, ")");
        return ans;
    }
    return std::nullopt;
}

std::vector<std::vector<parser::ast::ExprPtr>> Parser::ParseValueTuple() {
    Expect(parser::TokenKind::VALUES, "VALUES");
    std::vector<std::vector<parser::ast::ExprPtr>> ans;
    do {
        Expect(parser::TokenKind::LEFT_PAREN, "(");
        std::vector<parser::ast::ExprPtr> values;
        do {
            values.push_back(ParseExpression());
        } while (Match(parser::TokenKind::COMMA));
        Expect(parser::TokenKind::RIGHT_PAREN, ")");
        ans.push_back(std::move(values));
    } while (Match(parser::TokenKind::COMMA));
    return ans;
}

parser::ast::UpdateStatement Parser::ParseUpdateStatement() {
    parser::ast::UpdateStatement stmt;
    stmt.table = ParseTableName();
    stmt.assignments = ParseValuesAssignments();
    stmt.where = ParseWhere();
    return stmt;
}

std::vector<std::pair<std::string, parser::ast::ExprPtr>> Parser::ParseValuesAssignments() {
    Expect(parser::TokenKind::SET, "SET");
    std::vector<std::pair<std::string, parser::ast::ExprPtr>> ans;
    do {
        std::string field = ExpectIdentifier();
        Expect(parser::TokenKind::EQUAL, "=");
        parser::ast::ExprPtr value = ParseExpression();
        ans.emplace_back(std::move(field), std::move(value));
    } while (Match(parser::TokenKind::COMMA));
    return ans;
}

parser::ast::DeleteStatement Parser::ParseDeleteStatement() {
    Expect(parser::TokenKind::FROM, "FROM");
    parser::ast::DeleteStatement stmt;
    stmt.table = ParseTableName();
    stmt.where = ParseWhere();
    return stmt;
}

parser::ast::DropTableStatement Parser::ParseDrop() {
    Expect(parser::TokenKind::TABLE, "TABLE");
    parser::ast::DropTableStatement stmt;
    stmt.table = ParseTableName();
    return stmt;
}

parser::ast::CreateTableStatement Parser::ParseCreateTable() {
    Expect(parser::TokenKind::TABLE, "TABLE");
    parser::ast::CreateTableStatement stmt;
    stmt.table = ParseTableName();
    stmt.columns = ParseColumnsCreation();
    return stmt;
}

std::vector<parser::ast::ColumnDef> Parser::ParseColumnsCreation() {
    Expect(parser::TokenKind::LEFT_PAREN, "(");
    std::vector<parser::ast::ColumnDef> cols;
    do {
        cols.push_back(ParseColumnDef());
    } while (Match(parser::TokenKind::COMMA));
    Expect(parser::TokenKind::RIGHT_PAREN, ")");
    return cols;
}

parser::ast::ColumnDef Parser::ParseColumnDef() {
    parser::ast::ColumnDef col;
    const auto& nameTok = Peek();
    col.name = ExpectIdentifier();
    col.line = nameTok.GetLine();
    col.column = nameTok.GetColumn();
    col.typeName = ExpectIdentifier();

    if (Match(parser::TokenKind::LEFT_PAREN)) {
        const auto& sizeTok = Expect(parser::TokenKind::INTEGER, "type dimension/size");
        col.typeSize = static_cast<uint32_t>(std::stoul(sizeTok.GetLexem()));
        Expect(parser::TokenKind::RIGHT_PAREN, ")");
    }

    while (true) {
        if (Match(parser::TokenKind::PRIMARY)) {
            Expect(parser::TokenKind::KEY, "KEY");
            col.isPrimaryKey = true;
        } else if (Match(parser::TokenKind::NOT)) {
            Expect(parser::TokenKind::NULL_LIT, "NULL");
            col.isNotNull = true;
        } else if (Match(parser::TokenKind::NULL_LIT)) {
            col.isNotNull = false;
        } else {
            break;
        }
    }
    return col;
}

}  // namespace AxolotlDB::parser
