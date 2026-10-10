#include <parser/token.h>

namespace AxolotlDB::parser {

Token::Token(parser::TokenKind kind, uint16_t line, uint16_t column, const std::string& lexem)
    : kind_(kind), line_(line), column_(column), lexem_(lexem) {}
const std::string& Token::GetLexem() const {
    return lexem_;
}
uint16_t Token::GetColumn() const {
    return column_;
}
uint16_t Token::GetLine() const {
    return line_;
}
parser::TokenKind Token::GetKind() const {
    return kind_;
}

}  // namespace AxolotlDB::parser
