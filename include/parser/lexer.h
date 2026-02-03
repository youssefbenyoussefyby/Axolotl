#pragma once

#include <cstddef>
#include <cstdint>
#include<string>
#include<vector>
#include<parser/token.h>
#include<unordered_map>
#include<utils/diagnosticsink.h>

namespace AxolotlDB::parser
{

    class Lexer{
        public:
            explicit Lexer(std::string&,utils::DiagnosticSink&);
            [[nodiscard]] std::vector<parser::Token> ScanTokens();  


        private:
            size_t start_ = 0;
            size_t current_ = 0;
            uint16_t line_ = 1;
            uint16_t column_ = 1;
            std::string& src_;
            std::vector<parser::Token> tokens_;
            utils::DiagnosticSink& diagnosticSink_;
            bool isEnd();
            void ScanToken();
            char peek();
            void string();
            void number();
            char peekNext();
            void identifier();
            TokenKind lookupKeyword(const std::string& lexeme);
            char advance();
            void addToken(parser::TokenKind);
            void quoted_identifier();
            void ignore_single_line_comments();
            void ignore_multi_line_comments();
            bool isNextEnd();
            //std::unordered_map<std::string,parser::TokenKind>keyword_map;


    };

    
} // namespace AxolotlDB::parser
