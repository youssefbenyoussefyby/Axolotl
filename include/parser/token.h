#pragma once

#include <cstddef>
#include <cstdint>
#include<string>


namespace AxolotlDB::parser{
    enum class TokenKind{
        // Single-character punctuation
        LEFT_PAREN, RIGHT_PAREN,
        COMMA, DOT, SEMICOLON,
        PLUS, MINUS, STAR, SLASH,

        // One or two character operators (maximal munch)
        EQUAL,              // =
        BANG_EQUAL,         // != or <>  (map both lexemes to this one kind)
        LESS,               // 
        LESS_EQUAL,         // <=
        GREATER,            // >
        GREATER_EQUAL,      // >=

        // Vector distance operators
        VECTOR_L2,          // <->
        VECTOR_COSINE,      // <=>

        // Literals
        IDENTIFIER,
        QUOTED_IDENTIFIER,  // "my col" — case-preserving, distinct so parser skips keyword lookup
        STRING,             // 'text'
        INTEGER,            // 42
        FLOAT,              // 42.5, 42e10

        // Keywords — DDL
        CREATE, TABLE, DROP,
        PRIMARY, KEY,

        // Keywords — DML
        INSERT, INTO, VALUES,
        UPDATE, SET,
        DELETE, FROM,
        SELECT, AS,

        // Keywords — query clauses
        WHERE, JOIN, ON,
        GROUP, BY, HAVING,
        ORDER, ASC, DESC,
        LIMIT, OFFSET, DISTINCT,
        ALL,

        // Keywords — logical / literal
        AND, OR, NOT,
        NULL_LIT, TRUE_LIT, FALSE_LIT,

        // IS , LEFT JOIN , INNER JOIN
        IS,LEFT,INNER,RIGHT,CROSS,NATURAL,
        FULL,OUTER,USING,
        // Sentinel
        END_OF_FILE,
    };
    class Token{
        public:
            explicit Token(TokenKind,uint16_t,uint16_t,const std::string&);
            [[nodiscard]] TokenKind GetKind() const;
            [[nodiscard]] uint16_t GetLine() const;
            [[nodiscard]] uint16_t GetColumn() const ;
            [[nodiscard]] const std::string& GetLexem() const;
            
        private:
            TokenKind kind_;
            uint16_t line_ ;
            uint16_t column_;
            std::string lexem_;

    };

}