#include <gtest/gtest.h>
#include <parser/lexer.h>
#include <parser/token.h>
#include <utils/diagnosticsink.h>
#include <utils/errors.h>

#include <string>
#include <vector>

using namespace AxolotlDB;
using parser::TokenKind;

namespace {

// Helper to extract lexeme string from Token
inline const std::string& GetLexeme(const parser::Token& tok) {
    return tok.GetLexem();
}

// Helper to scan tokens from SQL text
inline std::vector<parser::Token> Scan(std::string sql, utils::DiagnosticSink& sink) {
    parser::Lexer lexer(sql, sink);
    return lexer.ScanTokens();
}

inline std::vector<parser::Token> Scan(std::string sql) {
    utils::DiagnosticSink sink;
    return Scan(std::move(sql), sink);
}

}  // namespace

// ===========================================================================
// 3.1 Lexer Tests from P3.md Checklist
// ===========================================================================

TEST(LexerTest, TokenCarriesLineAndColumn) {
    std::string sql =
        "SELECT\n"
        "  id, name\n"
        "FROM users;";

    utils::DiagnosticSink sink;
    auto tokens = Scan(sql, sink);

    ASSERT_FALSE(sink.hasError());
    // Expect: SELECT, id, comma, name, FROM, users, semicolon, EOF
    ASSERT_GE(tokens.size(), 7u);

    // Line 1: SELECT starts at line 1, column 1
    EXPECT_EQ(tokens[0].GetKind(), TokenKind::SELECT);
    EXPECT_EQ(tokens[0].GetLine(), 1u);
    EXPECT_EQ(tokens[0].GetColumn(), 1u);
    EXPECT_EQ(GetLexeme(tokens[0]), "SELECT");

    // Line 2: "  id" starts at column 3 (after 2 spaces)
    EXPECT_EQ(tokens[1].GetKind(), TokenKind::IDENTIFIER);
    EXPECT_EQ(tokens[1].GetLine(), 2u);
    EXPECT_EQ(tokens[1].GetColumn(), 3u);
    EXPECT_EQ(GetLexeme(tokens[1]), "id");

    // Line 2: "," at column 5
    EXPECT_EQ(tokens[2].GetKind(), TokenKind::COMMA);
    EXPECT_EQ(tokens[2].GetLine(), 2u);
    EXPECT_EQ(tokens[2].GetColumn(), 5u);

    // Line 2: "name" at column 7
    EXPECT_EQ(tokens[3].GetKind(), TokenKind::IDENTIFIER);
    EXPECT_EQ(tokens[3].GetLine(), 2u);
    EXPECT_EQ(tokens[3].GetColumn(), 7u);
    EXPECT_EQ(GetLexeme(tokens[3]), "name");

    // Line 3: "FROM" at column 1
    EXPECT_EQ(tokens[4].GetKind(), TokenKind::FROM);
    EXPECT_EQ(tokens[4].GetLine(), 3u);
    EXPECT_EQ(tokens[4].GetColumn(), 1u);

    // Line 3: "users" at column 6
    EXPECT_EQ(tokens[5].GetKind(), TokenKind::IDENTIFIER);
    EXPECT_EQ(tokens[5].GetLine(), 3u);
    EXPECT_EQ(tokens[5].GetColumn(), 6u);

    // Line 3: ";" at column 11
    EXPECT_EQ(tokens[6].GetKind(), TokenKind::SEMICOLON);
    EXPECT_EQ(tokens[6].GetLine(), 3u);
    EXPECT_EQ(tokens[6].GetColumn(), 11u);
}

TEST(LexerTest, KeywordsAreCaseInsensitive) {
    std::string sql =
        "select SELECT SeLeCt\n"
        "where WHERE WhErE\n"
        "create table drop insert into values update set delete from\n"
        "join on group by having order asc desc limit and or not null true false";

    utils::DiagnosticSink sink;
    auto tokens = Scan(sql, sink);
    ASSERT_FALSE(sink.hasError());

    EXPECT_EQ(tokens[0].GetKind(), TokenKind::SELECT);
    EXPECT_EQ(tokens[1].GetKind(), TokenKind::SELECT);
    EXPECT_EQ(tokens[2].GetKind(), TokenKind::SELECT);

    EXPECT_EQ(tokens[3].GetKind(), TokenKind::WHERE);
    EXPECT_EQ(tokens[4].GetKind(), TokenKind::WHERE);
    EXPECT_EQ(tokens[5].GetKind(), TokenKind::WHERE);

    // Verify DDL / DML keywords
    EXPECT_EQ(tokens[6].GetKind(), TokenKind::CREATE);
    EXPECT_EQ(tokens[7].GetKind(), TokenKind::TABLE);
    EXPECT_EQ(tokens[8].GetKind(), TokenKind::DROP);
    EXPECT_EQ(tokens[9].GetKind(), TokenKind::INSERT);
    EXPECT_EQ(tokens[10].GetKind(), TokenKind::INTO);
    EXPECT_EQ(tokens[11].GetKind(), TokenKind::VALUES);
    EXPECT_EQ(tokens[12].GetKind(), TokenKind::UPDATE);
    EXPECT_EQ(tokens[13].GetKind(), TokenKind::SET);
    EXPECT_EQ(tokens[14].GetKind(), TokenKind::DELETE);
    EXPECT_EQ(tokens[15].GetKind(), TokenKind::FROM);
}

TEST(LexerTest, QuotedIdentifierPreservesCase) {
    std::string sql = "\"CamelCaseColumn\" \"select\" \"spaced column name\"";

    utils::DiagnosticSink sink;
    auto tokens = Scan(sql, sink);
    ASSERT_FALSE(sink.hasError());

    ASSERT_GE(tokens.size(), 3u);

    EXPECT_EQ(tokens[0].GetKind(), TokenKind::QUOTED_IDENTIFIER);
    EXPECT_EQ(GetLexeme(tokens[0]), "\"CamelCaseColumn\"");

    // "select" inside quotes must NOT be treated as TokenKind::SELECT
    EXPECT_EQ(tokens[1].GetKind(), TokenKind::QUOTED_IDENTIFIER);
    EXPECT_EQ(GetLexeme(tokens[1]), "\"select\"");

    EXPECT_EQ(tokens[2].GetKind(), TokenKind::QUOTED_IDENTIFIER);
    EXPECT_EQ(GetLexeme(tokens[2]), "\"spaced column name\"");
}

TEST(LexerTest, EscapedQuoteInStringLiteral) {
    std::string sql = "'hello' 'it''s fine' '' 'quoted ''word'' here'";

    utils::DiagnosticSink sink;
    auto tokens = Scan(sql, sink);
    ASSERT_FALSE(sink.hasError());

    ASSERT_GE(tokens.size(), 4u);

    EXPECT_EQ(tokens[0].GetKind(), TokenKind::STRING);
    EXPECT_EQ(GetLexeme(tokens[0]), "'hello'");

    // Escaped '' inside string literal
    EXPECT_EQ(tokens[1].GetKind(), TokenKind::STRING);
    EXPECT_EQ(GetLexeme(tokens[1]), "'it''s fine'");

    // Empty string
    EXPECT_EQ(tokens[2].GetKind(), TokenKind::STRING);
    EXPECT_EQ(GetLexeme(tokens[2]), "''");

    EXPECT_EQ(tokens[3].GetKind(), TokenKind::STRING);
    EXPECT_EQ(GetLexeme(tokens[3]), "'quoted ''word'' here'");
}

TEST(LexerTest, IntegerVsFloatDisambiguation) {
    std::string sql = "42 42.5 0 0.001 42e10 1.5e-3";

    utils::DiagnosticSink sink;
    auto tokens = Scan(sql, sink);
    ASSERT_FALSE(sink.hasError());

    ASSERT_GE(tokens.size(), 6u);

    EXPECT_EQ(tokens[0].GetKind(), TokenKind::INTEGER);
    EXPECT_EQ(GetLexeme(tokens[0]), "42");

    EXPECT_EQ(tokens[1].GetKind(), TokenKind::FLOAT);
    EXPECT_EQ(GetLexeme(tokens[1]), "42.5");

    EXPECT_EQ(tokens[2].GetKind(), TokenKind::INTEGER);
    EXPECT_EQ(GetLexeme(tokens[2]), "0");

    EXPECT_EQ(tokens[3].GetKind(), TokenKind::FLOAT);
    EXPECT_EQ(GetLexeme(tokens[3]), "0.001");

    EXPECT_EQ(tokens[4].GetKind(), TokenKind::FLOAT);
    EXPECT_EQ(GetLexeme(tokens[4]), "42e10");

    EXPECT_EQ(tokens[5].GetKind(), TokenKind::FLOAT);
    EXPECT_EQ(GetLexeme(tokens[5]), "1.5e-3");
}

TEST(LexerTest, LineCountSurvivesBlockComment) {
    std::string sql =
        "SELECT /* block comment\n"
        "   spanning multiple\n"
        "   lines */ id,\n"
        "-- single line comment here\n"
        "name FROM t;";

    utils::DiagnosticSink sink;
    auto tokens = Scan(sql, sink);
    ASSERT_FALSE(sink.hasError());

    ASSERT_GE(tokens.size(), 5u);

    // SELECT on line 1
    EXPECT_EQ(tokens[0].GetKind(), TokenKind::SELECT);
    EXPECT_EQ(tokens[0].GetLine(), 1u);

    // id is after the 3-line block comment, on line 3
    EXPECT_EQ(tokens[1].GetKind(), TokenKind::IDENTIFIER);
    EXPECT_EQ(GetLexeme(tokens[1]), "id");
    EXPECT_EQ(tokens[1].GetLine(), 3u);

    // comma on line 3
    EXPECT_EQ(tokens[2].GetKind(), TokenKind::COMMA);
    EXPECT_EQ(tokens[2].GetLine(), 3u);

    // name is after the single line comment, on line 5
    EXPECT_EQ(tokens[3].GetKind(), TokenKind::IDENTIFIER);
    EXPECT_EQ(GetLexeme(tokens[3]), "name");
    EXPECT_EQ(tokens[3].GetLine(), 5u);

    // FROM on line 5
    EXPECT_EQ(tokens[4].GetKind(), TokenKind::FROM);
    EXPECT_EQ(tokens[4].GetLine(), 5u);
}

TEST(LexerTest, UnterminatedStringThrowsAtOpeningQuote) {
    // Check unterminated single-quoted string
    std::string sql = "SELECT 'unterminated string\nFROM t;";

    utils::DiagnosticSink sink;
    try {
        parser::Lexer lexer(sql, sink);
        (void)lexer.ScanTokens();
        // If it doesn't throw, DiagnosticSink must report the error with position
        EXPECT_TRUE(sink.hasError());
        const auto& diags = sink.getDiagnostics();
        ASSERT_FALSE(diags.empty());
        EXPECT_EQ(diags[0].line, 1u);
        EXPECT_EQ(diags[0].column, 8u);  // Opening quote ' starts at column 8
    } catch (const utils::AxolotException&) {
        // If implemented via exception throwing as specified in P3.md §3.5
        SUCCEED();
    }
}

TEST(LexerTest, OperatorsAndPunctuation) {
    std::string sql = "= <> != < <= > >= + - * / ( ) , . ; <-> <=>";

    utils::DiagnosticSink sink;
    auto tokens = Scan(sql, sink);
    ASSERT_FALSE(sink.hasError());

    ASSERT_GE(tokens.size(), 18u);

    EXPECT_EQ(tokens[0].GetKind(), TokenKind::EQUAL);
    EXPECT_EQ(tokens[1].GetKind(), TokenKind::BANG_EQUAL);  // <>
    EXPECT_EQ(tokens[2].GetKind(), TokenKind::BANG_EQUAL);  // !=
    EXPECT_EQ(tokens[3].GetKind(), TokenKind::LESS);
    EXPECT_EQ(tokens[4].GetKind(), TokenKind::LESS_EQUAL);
    EXPECT_EQ(tokens[5].GetKind(), TokenKind::GREATER);
    EXPECT_EQ(tokens[6].GetKind(), TokenKind::GREATER_EQUAL);
    EXPECT_EQ(tokens[7].GetKind(), TokenKind::PLUS);
    EXPECT_EQ(tokens[8].GetKind(), TokenKind::MINUS);
    EXPECT_EQ(tokens[9].GetKind(), TokenKind::STAR);
    EXPECT_EQ(tokens[10].GetKind(), TokenKind::SLASH);
    EXPECT_EQ(tokens[11].GetKind(), TokenKind::LEFT_PAREN);
    EXPECT_EQ(tokens[12].GetKind(), TokenKind::RIGHT_PAREN);
    EXPECT_EQ(tokens[13].GetKind(), TokenKind::COMMA);
    EXPECT_EQ(tokens[14].GetKind(), TokenKind::DOT);
    EXPECT_EQ(tokens[15].GetKind(), TokenKind::SEMICOLON);
    EXPECT_EQ(tokens[16].GetKind(), TokenKind::VECTOR_L2);      // <->
    EXPECT_EQ(tokens[17].GetKind(), TokenKind::VECTOR_COSINE);  // <=>
}
