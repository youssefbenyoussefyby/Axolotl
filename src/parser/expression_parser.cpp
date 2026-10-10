#include <parser/parser.h>
#include <utils/errors.h>

namespace AxolotlDB::parser {
static int GetBinaryPrecedence(parser::TokenKind kind) {
    switch (kind) {
        case parser::TokenKind::OR:
            return 1;
        case parser::TokenKind::AND:
            return 2;
        case parser::TokenKind::EQUAL:
        case parser::TokenKind::BANG_EQUAL:
        case parser::TokenKind::LESS:
        case parser::TokenKind::LESS_EQUAL:
        case parser::TokenKind::GREATER:
        case parser::TokenKind::GREATER_EQUAL:
        case parser::TokenKind::IS:
            return 4;
        case parser::TokenKind::PLUS:
        case parser::TokenKind::MINUS:
        case parser::TokenKind::VECTOR_L2:
        case parser::TokenKind::VECTOR_COSINE:
            return 5;
        case parser::TokenKind::STAR:
        case parser::TokenKind::SLASH:
            return 6;
        default:
            return 0;
    }
}

parser::ast::ExprPtr Parser::ParseExpression(int minPrecedence) {
    parser::ast::ExprPtr left = ParsePrimary();

    while (true) {
        int prec = GetBinaryPrecedence(Peek().GetKind());
        if (prec <= minPrecedence) {
            break;
        }

        parser::Token opTok = Advance();
        parser::ast::BinaryOperator op;
        switch (opTok.GetKind()) {
            case parser::TokenKind::PLUS:
                op = parser::ast::BinaryOperator::ADD;
                break;
            case parser::TokenKind::MINUS:
                op = parser::ast::BinaryOperator::SUBTRACT;
                break;
            case parser::TokenKind::STAR:
                op = parser::ast::BinaryOperator::MULTIPLY;
                break;
            case parser::TokenKind::SLASH:
                op = parser::ast::BinaryOperator::DIVIDE;
                break;
            case parser::TokenKind::EQUAL:
                op = parser::ast::BinaryOperator::EQUAL;
                break;
            case parser::TokenKind::BANG_EQUAL:
                op = parser::ast::BinaryOperator::NOT_EQUAL;
                break;
            case parser::TokenKind::LESS:
                op = parser::ast::BinaryOperator::LESS;
                break;
            case parser::TokenKind::LESS_EQUAL:
                op = parser::ast::BinaryOperator::LESS_EQUAL;
                break;
            case parser::TokenKind::GREATER:
                op = parser::ast::BinaryOperator::GREATER;
                break;
            case parser::TokenKind::GREATER_EQUAL:
                op = parser::ast::BinaryOperator::GREATER_EQUAL;
                break;
            case parser::TokenKind::AND:
                op = parser::ast::BinaryOperator::AND;
                break;
            case parser::TokenKind::OR:
                op = parser::ast::BinaryOperator::OR;
                break;
            case parser::TokenKind::VECTOR_L2:
                op = parser::ast::BinaryOperator::VECTOR_L2;
                break;
            case parser::TokenKind::VECTOR_COSINE:
                op = parser::ast::BinaryOperator::VECTOR_COSINE;
                break;
            default:
                return left;
        }

        parser::ast::ExprPtr right = ParseExpression(prec);
        left = std::make_unique<parser::ast::Expr>(
            parser::ast::BinaryOp(op, std::move(left), std::move(right)), opTok.GetLine(),
            opTok.GetColumn());
    }

    return left;
}

parser::ast::ExprPtr Parser::ParsePrimary() {
    const auto& tok = Peek();
    uint16_t line = tok.GetLine();
    uint16_t col = tok.GetColumn();

    if (Match(parser::TokenKind::INTEGER)) {
        return std::make_unique<parser::ast::Expr>(parser::ast::Literal(std::stoll(tok.GetLexem())),
                                                   line, col);
    }
    if (Match(parser::TokenKind::FLOAT)) {
        return std::make_unique<parser::ast::Expr>(parser::ast::Literal(std::stod(tok.GetLexem())),
                                                   line, col);
    }
    if (Match(parser::TokenKind::STRING)) {
        return std::make_unique<parser::ast::Expr>(parser::ast::Literal(tok.GetLexem()), line, col);
    }
    if (Match(parser::TokenKind::TRUE_LIT)) {
        return std::make_unique<parser::ast::Expr>(parser::ast::Literal(true), line, col);
    }
    if (Match(parser::TokenKind::FALSE_LIT)) {
        return std::make_unique<parser::ast::Expr>(parser::ast::Literal(false), line, col);
    }
    if (Match(parser::TokenKind::NULL_LIT)) {
        return std::make_unique<parser::ast::Expr>(parser::ast::Literal(nullptr), line, col);
    }

    if (Match(parser::TokenKind::NOT)) {
        parser::ast::ExprPtr sub = ParseExpression(3);
        return std::make_unique<parser::ast::Expr>(
            parser::ast::UnaryOp(parser::ast::UnaryOperator::NOT, std::move(sub)), line, col);
    }

    if (Match(parser::TokenKind::MINUS)) {
        parser::ast::ExprPtr sub = ParseExpression(7);
        return std::make_unique<parser::ast::Expr>(
            parser::ast::UnaryOp(parser::ast::UnaryOperator::NEGATE, std::move(sub)), line, col);
    }

    if (Match(parser::TokenKind::LEFT_PAREN)) {
        parser::ast::ExprPtr expr = ParseExpression(0);
        Expect(parser::TokenKind::RIGHT_PAREN, ")");
        return expr;
    }

    if (Match(parser::TokenKind::STAR)) {
        return std::make_unique<parser::ast::Expr>(parser::ast::Star{}, line, col);
    }

    if (check(parser::TokenKind::IDENTIFIER) || check(parser::TokenKind::QUOTED_IDENTIFIER)) {
        std::string id = Advance().GetLexem();

        if (Match(parser::TokenKind::LEFT_PAREN)) {
            std::vector<parser::ast::ExprPtr> args;
            if (!check(parser::TokenKind::RIGHT_PAREN)) {
                do {
                    if (Match(parser::TokenKind::STAR)) {
                        args.push_back(
                            std::make_unique<parser::ast::Expr>(parser::ast::Star{}, line, col));
                    } else {
                        args.push_back(ParseExpression(0));
                    }
                } while (Match(parser::TokenKind::COMMA));
            }
            Expect(parser::TokenKind::RIGHT_PAREN, ")");
            return std::make_unique<parser::ast::Expr>(
                parser::ast::FunctionCall(id, std::move(args)), line, col);
        }

        if (Match(parser::TokenKind::DOT)) {
            if (Match(parser::TokenKind::STAR)) {
                return std::make_unique<parser::ast::Expr>(parser::ast::Star{id}, line, col);
            }
            std::string colName = ExpectIdentifier();
            return std::make_unique<parser::ast::Expr>(parser::ast::ColumnRef(id, colName), line,
                                                       col);
        }

        return std::make_unique<parser::ast::Expr>(parser::ast::ColumnRef(id), line, col);
    }

    Error("Unexpected token in expression: " + tok.GetLexem());
    return nullptr;
}
}  // namespace AxolotlDB::parser
