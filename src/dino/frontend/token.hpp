#pragma once

#include <string>

namespace dino::frontend {

enum class AccessModifier {
    Private,
    Public,
};

enum class TokenType {
    EndOfFile,
    Invalid,

    Identifier,
    Number,
    String,
    Character,

    At,
    Hash,
    Comma,
    Dot,
    Colon,
    Semicolon,
    Question,

    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,

    Plus,
    Minus,
    Star,
    Slash,
    Percent,

    PlusPlus,
    MinusMinus,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    PercentAssign,

    Assign,
    Equal,
    NotEqual,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,

    And,
    Or,
    Xor,
    Not,
    AndAnd,
    OrOr,
    Tilde,

    ShiftLeft,
    ShiftRight,
    ShiftLeftAssign,
    ShiftRightAssign,
    AndAssign,
    OrAssign,
    XorAssign,

    Arrow,
    Ellipsis,

    KwInclude,
    KwPublic,
    KwPrivate,

    KwInt8,
    KwInt16,
    KwInt32,
    KwInt64,
    KwUint8,
    KwUint16,
    KwUint32,
    KwUint64,
    KwFloat,
    KwDouble,
    KwChar,
    KwVoid,
    KwBool,

    KwStruct,
    KwStatic,
    KwIf,
    KwElse,
    KwWhile,
    KwFor,
    KwIn,
    KwMatch,
    KwCase,
    KwDefault,
    KwYield,
    KwReturn,
    KwFallthrough,
    KwNew,
    KwDelete,
    KwTemplate,
    KwTypename,
    KwThis,
    KwTrue,
    KwFalse,
};

struct SourceLocation {
    std::string file;
    size_t line = 1;
    size_t column = 1;
};

struct Token {
    TokenType type = TokenType::Invalid;
    std::string lexeme;
    SourceLocation location;
};

const char* to_string(TokenType type);
const char* to_string(AccessModifier access);

} // namespace dino::frontend
