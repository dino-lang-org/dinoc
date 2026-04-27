#include "dino/frontend/token.hpp"

namespace dino::frontend {

const char* to_string(TokenType type) {
    switch (type) {
    case TokenType::EndOfFile: return "EndOfFile";
    case TokenType::Invalid: return "Invalid";
    case TokenType::Identifier: return "Identifier";
    case TokenType::Number: return "Number";
    case TokenType::String: return "String";
    case TokenType::Character: return "Character";
    case TokenType::At: return "At";
    case TokenType::Hash: return "Hash";
    case TokenType::Comma: return "Comma";
    case TokenType::Dot: return "Dot";
    case TokenType::Colon: return "Colon";
    case TokenType::Semicolon: return "Semicolon";
    case TokenType::Question: return "Question";
    case TokenType::LParen: return "LParen";
    case TokenType::RParen: return "RParen";
    case TokenType::LBrace: return "LBrace";
    case TokenType::RBrace: return "RBrace";
    case TokenType::LBracket: return "LBracket";
    case TokenType::RBracket: return "RBracket";
    case TokenType::Plus: return "Plus";
    case TokenType::Minus: return "Minus";
    case TokenType::Star: return "Star";
    case TokenType::Slash: return "Slash";
    case TokenType::Percent: return "Percent";
    case TokenType::PlusPlus: return "PlusPlus";
    case TokenType::MinusMinus: return "MinusMinus";
    case TokenType::PlusAssign: return "PlusAssign";
    case TokenType::MinusAssign: return "MinusAssign";
    case TokenType::StarAssign: return "StarAssign";
    case TokenType::SlashAssign: return "SlashAssign";
    case TokenType::PercentAssign: return "PercentAssign";
    case TokenType::Assign: return "Assign";
    case TokenType::Equal: return "Equal";
    case TokenType::NotEqual: return "NotEqual";
    case TokenType::Less: return "Less";
    case TokenType::Greater: return "Greater";
    case TokenType::LessEqual: return "LessEqual";
    case TokenType::GreaterEqual: return "GreaterEqual";
    case TokenType::And: return "And";
    case TokenType::Or: return "Or";
    case TokenType::Xor: return "Xor";
    case TokenType::Not: return "Not";
    case TokenType::AndAnd: return "AndAnd";
    case TokenType::OrOr: return "OrOr";
    case TokenType::Tilde: return "Tilde";
    case TokenType::ShiftLeft: return "ShiftLeft";
    case TokenType::ShiftRight: return "ShiftRight";
    case TokenType::ShiftLeftAssign: return "ShiftLeftAssign";
    case TokenType::ShiftRightAssign: return "ShiftRightAssign";
    case TokenType::AndAssign: return "AndAssign";
    case TokenType::OrAssign: return "OrAssign";
    case TokenType::XorAssign: return "XorAssign";
    case TokenType::Arrow: return "Arrow";
    case TokenType::Ellipsis: return "Ellipsis";
    case TokenType::KwInclude: return "KwInclude";
    case TokenType::KwPublic: return "KwPublic";
    case TokenType::KwPrivate: return "KwPrivate";
    case TokenType::KwInt8: return "KwInt8";
    case TokenType::KwInt16: return "KwInt16";
    case TokenType::KwInt32: return "KwInt32";
    case TokenType::KwInt64: return "KwInt64";
    case TokenType::KwUint8: return "KwUint8";
    case TokenType::KwUint16: return "KwUint16";
    case TokenType::KwUint32: return "KwUint32";
    case TokenType::KwUint64: return "KwUint64";
    case TokenType::KwFloat: return "KwFloat";
    case TokenType::KwDouble: return "KwDouble";
    case TokenType::KwChar: return "KwChar";
    case TokenType::KwVoid: return "KwVoid";
    case TokenType::KwBool: return "KwBool";
    case TokenType::KwStruct: return "KwStruct";
    case TokenType::KwStatic: return "KwStatic";
    case TokenType::KwIf: return "KwIf";
    case TokenType::KwElse: return "KwElse";
    case TokenType::KwWhile: return "KwWhile";
    case TokenType::KwFor: return "KwFor";
    case TokenType::KwIn: return "KwIn";
    case TokenType::KwMatch: return "KwMatch";
    case TokenType::KwCase: return "KwCase";
    case TokenType::KwDefault: return "KwDefault";
    case TokenType::KwYield: return "KwYield";
    case TokenType::KwReturn: return "KwReturn";
    case TokenType::KwFallthrough: return "KwFallthrough";
    case TokenType::KwNew: return "KwNew";
    case TokenType::KwDelete: return "KwDelete";
    case TokenType::KwTemplate: return "KwTemplate";
    case TokenType::KwTypename: return "KwTypename";
    case TokenType::KwThis: return "KwThis";
    case TokenType::KwTrue: return "KwTrue";
    case TokenType::KwFalse: return "KwFalse";
    case TokenType::KwNullptr: return "KwNullptr";
    }
    return "Unknown";
}

const char* to_string(AccessModifier access) {
    return access == AccessModifier::Public ? "public" : "private";
}

} // namespace dino::frontend
