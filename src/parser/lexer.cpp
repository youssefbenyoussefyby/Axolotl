#include <parser/lexer.h>

#include <algorithm>
#include <cctype>

namespace AxolotlDB::parser {

Lexer::Lexer(std::string& src, utils::DiagnosticSink& diagnosticSink)
    : src_(src), diagnosticSink_(diagnosticSink) { /*ScanTokens();*/ }
bool Lexer::isEnd() {
    return current_ >= src_.size();
}
bool Lexer::isNextEnd() {
    return (current_ + 1) >= src_.size();
}

char Lexer::peek() {
    if (isEnd())
        return '\0';
    return src_[current_];
}

char Lexer::peekNext() {
    if (current_ + 1 >= src_.size())
        return '\0';
    return src_[current_ + 1];
}

std::vector<parser::Token> Lexer::ScanTokens() {
    while (!isEnd()) {
        start_ = current_;
        ScanToken();
    }
    tokens_.push_back(parser::Token(parser::TokenKind::END_OF_FILE, line_, column_, ""));
    return tokens_;
}
char Lexer::advance() {
    char c = src_[current_++];
    if (c == '\n') {
        line_++;
        column_ = 0;
    }
    return c;
}
void Lexer::addToken(parser::TokenKind kind) {
    tokens_.push_back(parser::Token(kind, line_, column_, src_.substr(start_, current_ - start_)));
}
void Lexer::ScanToken() {
    char c = advance();
    bool isComment = false;
    bool isSTring = false;
    switch (c) {
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            break;
        case '(':
            addToken(parser::TokenKind::LEFT_PAREN);
            break;
        case ')':
            addToken(parser::TokenKind::RIGHT_PAREN);
            break;
        case ',':
            addToken(parser::TokenKind::COMMA);
            break;
        case '.':
            addToken(parser::TokenKind::DOT);
            break;
        case ';':
            addToken(parser::TokenKind::SEMICOLON);
            break;
        case '+':
            addToken(parser::TokenKind::PLUS);
            break;
        case '-':
            if (peek() == '-')
                ignore_single_line_comments(), isComment = true;
            else
                addToken(parser::TokenKind::MINUS);
            break;
        case '*':
            addToken(parser::TokenKind::STAR);
            break;
        case '/':
            if (peek() == '*')
                ignore_multi_line_comments(), isComment = true;
            else
                addToken(parser::TokenKind::SLASH);
            break;
        case '=':
            addToken(parser::TokenKind::EQUAL);
            break;
        case '!':
            if (peek() == '=') {
                advance();
                addToken(parser::TokenKind::BANG_EQUAL);
            } else {
                diagnosticSink_.addDiagnostic({"Unexpected character '!'", line_, column_, "!"});
            }
            break;

        case '<':
            if (peek() == '=') {
                advance();
                if (peek() == '>') {
                    advance();
                    addToken(parser::TokenKind::VECTOR_COSINE);
                } else {
                    addToken(parser::TokenKind::LESS_EQUAL);
                }
            } else if (peek() == '>') {
                advance();
                addToken(parser::TokenKind::BANG_EQUAL);
            } else if (peek() == '-') {
                advance();
                if (peek() == '>') {
                    advance();
                    addToken(parser::TokenKind::VECTOR_L2);
                } else {
                    diagnosticSink_.addDiagnostic(
                        {"Unexpected character", line_, column_, std::string(1, c)});
                }

            } else {
                addToken(parser::TokenKind::LESS);
            }
            break;

        case '>':
            if (peek() == '=') {
                advance();
                addToken(parser::TokenKind::GREATER_EQUAL);
            } else {
                addToken(parser::TokenKind::GREATER);
            }
            break;
        case '\'':
            string();
            isSTring = true;
            break;
        case '"':
            quoted_identifier();
            break;
        case '_':
            identifier();
            break;

        default:
            if (std::isdigit(static_cast<unsigned char>(c)))
                number();
            else if (std::isalpha(c))
                identifier();
            else
                diagnosticSink_.addDiagnostic(
                    {"Unexpected character", line_, column_, std::string(1, c)});

            break;
    }
    if (!isComment)
        column_ += current_ - start_ + (isSTring == 1);
}

void Lexer::number() {
    bool isFloat = false;
    bool e = false;
    while (!isEnd() && std::isdigit(static_cast<unsigned char>(peek())))
        advance();
    if (!isEnd() && peek() == '.' && std::isdigit(static_cast<unsigned char>(peekNext()))) {
        isFloat = true;
        advance();
        while (!isEnd() && std::isdigit(static_cast<unsigned char>(peek())))
            advance();
    }
    if (!isEnd() && (peek() == 'e' || peek() == 'E')) {
        char sign = peekNext();
        size_t lookahead = current_ + 1;
        bool signPresent = (sign == '+' || sign == '-');
        char afterSign = signPresent && lookahead + 1 < src_.size() ? src_[lookahead + 1] : '\0';

        if (std::isdigit(static_cast<unsigned char>(sign)) ||
            (signPresent && std::isdigit(static_cast<unsigned char>(afterSign)))) {
            isFloat = true;
            advance();
            if (peek() == '+' || peek() == '-')
                advance();
            while (!isEnd() && std::isdigit(static_cast<unsigned char>(peek())))
                advance();
        }
    }
    if (isFloat)
        addToken(parser::TokenKind::FLOAT);
    else
        addToken(parser::TokenKind::INTEGER);
}

void Lexer::string() {
    while (!isEnd() && peek() != '\n') {
        if (peek() == '\'')
            if (!isNextEnd())
                if (peekNext() != '\'')
                    break;
                else
                    advance(), advance();
            else
                break;
        else
            advance();
    }

    if (isEnd() || peek() == '\n') {
        diagnosticSink_.addDiagnostic({"Unterminated string literal", line_, column_, ""});
        return;
    }
    start_++;
    addToken(parser::TokenKind::STRING);
    advance();
}
void Lexer::quoted_identifier() {
    while (!isEnd() && peek() != '"' && peek() != '\n')
        advance();

    if (isEnd() || peek() == '\n') {
        diagnosticSink_.addDiagnostic({"Unterminated string literal", line_, column_, ""});
        return;
    }
    advance();
    addToken(parser::TokenKind::QUOTED_IDENTIFIER);
}

void Lexer::identifier() {
    while (!isEnd() && (peek() == '_' || std::isdigit(static_cast<unsigned char>(peek())) ||
                        std::isalpha(static_cast<unsigned char>(peek()))))
        advance();
    std::string lexeme = src_.substr(start_, current_ - start_);
    std::string lowered = lexeme;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    addToken(lookupKeyword(lowered));
}

parser::TokenKind Lexer::lookupKeyword(const std::string& lexeme) {
    static const std::unordered_map<std::string, TokenKind> keyword_map = {
        {"create", TokenKind::CREATE},
        {"table", TokenKind::TABLE},
        {"drop", TokenKind::DROP},
        {"primary", TokenKind::PRIMARY},
        {"key", TokenKind::KEY},
        {"insert", TokenKind::INSERT},
        {"into", TokenKind::INTO},
        {"values", TokenKind::VALUES},
        {"update", TokenKind::UPDATE},
        {"set", TokenKind::SET},
        {"delete", TokenKind::DELETE},
        {"from", TokenKind::FROM},
        {"select", TokenKind::SELECT},
        {"as", TokenKind::AS},
        {"where", TokenKind::WHERE},
        {"join", TokenKind::JOIN},
        {"on", TokenKind::ON},
        {"group", TokenKind::GROUP},
        {"by", TokenKind::BY},
        {"having", TokenKind::HAVING},
        {"order", TokenKind::ORDER},
        {"asc", TokenKind::ASC},
        {"desc", TokenKind::DESC},
        {"limit", TokenKind::LIMIT},
        {"offset", TokenKind::OFFSET},
        {"and", TokenKind::AND},
        {"or", TokenKind::OR},
        {"not", TokenKind::NOT},
        {"null", TokenKind::NULL_LIT},
        {"true", TokenKind::TRUE_LIT},
        {"false", TokenKind::FALSE_LIT},
        {"is", TokenKind::IS},
        {"left", TokenKind::LEFT},
        {"inner", TokenKind::INNER},
        {"distinct", TokenKind::DISTINCT},
        {"all", TokenKind::ALL},
        {"cross", TokenKind::CROSS},
        {"natural", TokenKind::NATURAL},
        {"right", TokenKind::RIGHT},
        {"full", TokenKind::FULL},
        {"outer", TokenKind::OUTER},
        {"using", TokenKind::USING},

    };

    auto it = keyword_map.find(lexeme);
    return it == keyword_map.end() ? TokenKind::IDENTIFIER : it->second;
}

void Lexer::ignore_single_line_comments() {
    advance();
    while (!isEnd() && peek() != '\n')
        advance();
}

void Lexer::ignore_multi_line_comments() {
    advance();
    while (!isEnd() && !isNextEnd() && !(peek() == '*' && peekNext() == '/'))
        advance();
    if (isEnd() || isNextEnd()) {
        diagnosticSink_.addDiagnostic({"Comment in wrang format", line_, column_, ""});
        return;
    }

    advance();
    advance();
}

}  // namespace AxolotlDB::parser
