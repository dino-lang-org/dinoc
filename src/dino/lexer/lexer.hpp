#pragma once

namespace dino::lex {
    enum class TokenType {
        /// <!-- Directives -->
        DIRECTIVE_BEGIN, // @
        INCLUDE_DIRECTIVE, // include

        /// <!-- builtin types -->
        KW_INT8, // int8
        KW_INT16, // int16
        KW_INT32, // int32
        KW_INT64, // int64
        KW_UINT8, // uint8
        KW_UINT16, // uint16
        KW_UINT32, // uint32
        KW_UINT64, // uint64
        KW_FLOAT, // float
        KW_DOUBLE, // double
        KW_BOOL, // bool
        KW_CHAR, // char
        KW_VOID, // void
        KW_AUTO, // auto

        /// <!-- Declaration keywords -->
        KW_PUBLIC,
        KW_PRIVATE,
        KW_PROTECTED,
        KW_STATIC,
        KW_CONST,
        KW_INLINE,
        KW_EXTERN,
        KW_TEMPLATE,
        KW_TYPENAME,
        KW_STRUCT,
        KW_ENUM,

        /// <!-- Flow control keywords -->
        KW_IF,
        KW_ELSE,
        KW_FOR,
        KW_WHILE,
        KW_DO,
        KW_MATCH,
        KW_CASE,
        KW_DEFAULT,
        KW_YIELD,
        KW_FALLTHROUGH,
        KW_BREAK,
        KW_CONTINUE,
        KW_RETURN,
        KW_IN,

        /// <!-- Helper keywords -->
        KW_THIS,
        KW_TRUE,
        KW_FALSE,
        KW_NULLPTR,
        KW_SIZEOF,
        KW_NEW,
        KW_DELETE,

        /// <!-- Literals -->
        INTEGER_LITERAL,
        FLOAT_LITERAL,
        STRING_LITERAL,
        CHAR_LITERAL,
        BOOL_LITERAL,
        NULLPTR_LITERAL,

        /// <!-- Identifier -->
        IDENTIFIER,

        /// <!-- Arithmetic operators -->
        OP_PLUS,
        OP_MINUS,
        OP_STAR,
        OP_SLASH,
        OP_PERCENT,
        OP_PLUS_PLUS,
        OP_MINUS_MINUS,
        OP_PLUS_ASSIGN,
        OP_MINUS_ASSIGN,
        OP_STAR_ASSIGN,
        OP_SLASH_ASSIGN,
        OP_PERCENT_ASSIGN,

        /// <!-- Equal operators -->
        OP_EQ,
        OP_NE,
        OP_LT,
        OP_GT,
        OP_LE,
        OP_GE,

        /// <!-- Logical operators -->
        OP_AND,
        OP_OR,
        OP_NOT,
        OP_AND_AND,
        OP_OR_OR,

        /// <!-- Assignment operator -->
        OP_ASSIGN,

        /// <!-- Byteeach operators -->
        OP_BIT_AND,
        OP_BIT_OR,
        OP_BIT_XOR,
        OP_BIT_NOT,
        OP_SHL,
        OP_SHR,
        OP_BIT_AND_ASSIGN,
        OP_BIT_OR_ASSIGN,
        OP_BIT_XOR_ASSIGN,
        OP_SHL_ASSIGN,
        OP_SHR_ASSIGN,

        /// <!-- Access operators -->
        OP_DOT, // .
        OP_ARROW, // >
        OP_QUESTION, // ?
        OP_COLON, // ?x2
        OP_SEMICOLON, // ;
        OP_COMMA, // ,
        OP_LPAREN, // (
        OP_RPAREN, // )
        OP_LBRACE, // [
        OP_RBRACE, // ]
        OP_LBRACKET, // {
        OP_RBRACKET, // }
        OP_TILDE,  // ~

        /// <!-- Template specification -->
        OP_ANGLE_LT, // <
        OP_ANGLE_GT, // >

        /// <!-- Utility -->
        WHITESPACE, // ' '
        COMMENT_SINGLE, // / ?
        COMMENT_MULTI, // /* */ ?
        NEWLINE, // \n
        END_OF_FILE, // EOF
        UNKNOWN, // Something other
    };

#ifndef NDEBUG
    #include "lexer_debug_utils.inl"
#endif
}