#include "dino/frontend/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace dino::frontend {

Lexer::Lexer(std::string file, std::string source)
    : file_(std::move(file)), source_(std::move(source)) {}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    while (true) {
        skip_whitespace_and_comments();
        const size_t start = pos_;
        const size_t start_line = line_;
        const size_t start_column = column_;

        if (is_at_end()) {
            out.push_back(make_token(TokenType::EndOfFile, start, start_line, start_column));
            break;
        }

        const char c = peek();
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            out.push_back(scan_identifier_or_keyword(start, start_line, start_column));
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            out.push_back(scan_number(start, start_line, start_column));
            continue;
        }
        if (c == '"') {
            out.push_back(scan_string(start, start_line, start_column));
            continue;
        }
        if (c == '\'') {
            out.push_back(scan_character(start, start_line, start_column));
            continue;
        }

        out.push_back(scan_operator_or_punct(start, start_line, start_column));
    }
    return out;
}

bool Lexer::is_at_end() const { return pos_ >= source_.size(); }

char Lexer::peek(size_t offset) const {
    const size_t p = pos_ + offset;
    return p < source_.size() ? source_[p] : '\0';
}

char Lexer::advance() {
    if (is_at_end()) {
        return '\0';
    }
    const char c = source_[pos_++];
    if (c == '\n') {
        ++line_;
        column_ = 1;
    } else {
        ++column_;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (peek() != expected) {
        return false;
    }
    advance();
    return true;
}

void Lexer::skip_whitespace_and_comments() {
    while (!is_at_end()) {
        const char c = peek();
        if (std::isspace(static_cast<unsigned char>(c))) {
            advance();
            continue;
        }
        if (c == '/' && peek(1) == '/') {
            while (!is_at_end() && peek() != '\n') {
                advance();
            }
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            advance();
            advance();
            while (!is_at_end()) {
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    break;
                }
                advance();
            }
            continue;
        }
        break;
    }
}

Token Lexer::make_token(TokenType type, size_t start, size_t start_line, size_t start_column) const {
    Token token;
    token.type = type;
    token.lexeme = source_.substr(start, pos_ - start);
    token.location = SourceLocation {file_, start_line, start_column};
    return token;
}

Token Lexer::scan_identifier_or_keyword(size_t start, size_t start_line, size_t start_column) {
    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') {
        advance();
    }
    Token token = make_token(TokenType::Identifier, start, start_line, start_column);
    token.type = identifier_keyword(token.lexeme);
    return token;
}

Token Lexer::scan_number(size_t start, size_t start_line, size_t start_column) {
    while (std::isdigit(static_cast<unsigned char>(peek()))) {
        advance();
    }
    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
        advance();
        while (std::isdigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }
    return make_token(TokenType::Number, start, start_line, start_column);
}

Token Lexer::scan_string(size_t start, size_t start_line, size_t start_column) {
    advance();
    while (!is_at_end()) {
        if (peek() == '\\') {
            advance();
            if (!is_at_end()) {
                advance();
            }
            continue;
        }
        if (peek() == '"') {
            advance();
            return make_token(TokenType::String, start, start_line, start_column);
        }
        advance();
    }
    return make_token(TokenType::Invalid, start, start_line, start_column);
}

Token Lexer::scan_character(size_t start, size_t start_line, size_t start_column) {
    advance();
    if (!is_at_end() && peek() == '\\') {
        advance();
        if (!is_at_end()) {
            advance();
        }
    } else if (!is_at_end()) {
        advance();
    }
    if (match('\'')) {
        return make_token(TokenType::Character, start, start_line, start_column);
    }
    return make_token(TokenType::Invalid, start, start_line, start_column);
}

Token Lexer::scan_operator_or_punct(size_t start, size_t start_line, size_t start_column) {
    const char c = advance();
    switch (c) {
    case '@': return make_token(TokenType::At, start, start_line, start_column);
    case '#': return make_token(TokenType::Hash, start, start_line, start_column);
    case ',': return make_token(TokenType::Comma, start, start_line, start_column);
    case '.':
        if (match('.') && match('.')) {
            return make_token(TokenType::Ellipsis, start, start_line, start_column);
        }
        return make_token(TokenType::Dot, start, start_line, start_column);
    case ':': return make_token(TokenType::Colon, start, start_line, start_column);
    case ';': return make_token(TokenType::Semicolon, start, start_line, start_column);
    case '?': return make_token(TokenType::Question, start, start_line, start_column);
    case '(': return make_token(TokenType::LParen, start, start_line, start_column);
    case ')': return make_token(TokenType::RParen, start, start_line, start_column);
    case '{': return make_token(TokenType::LBrace, start, start_line, start_column);
    case '}': return make_token(TokenType::RBrace, start, start_line, start_column);
    case '[': return make_token(TokenType::LBracket, start, start_line, start_column);
    case ']': return make_token(TokenType::RBracket, start, start_line, start_column);
    case '+':
        if (match('+')) return make_token(TokenType::PlusPlus, start, start_line, start_column);
        if (match('=')) return make_token(TokenType::PlusAssign, start, start_line, start_column);
        return make_token(TokenType::Plus, start, start_line, start_column);
    case '-':
        if (match('-')) return make_token(TokenType::MinusMinus, start, start_line, start_column);
        if (match('>')) return make_token(TokenType::Arrow, start, start_line, start_column);
        if (match('=')) return make_token(TokenType::MinusAssign, start, start_line, start_column);
        return make_token(TokenType::Minus, start, start_line, start_column);
    case '*':
        if (match('=')) return make_token(TokenType::StarAssign, start, start_line, start_column);
        return make_token(TokenType::Star, start, start_line, start_column);
    case '/':
        if (match('=')) return make_token(TokenType::SlashAssign, start, start_line, start_column);
        return make_token(TokenType::Slash, start, start_line, start_column);
    case '%':
        if (match('=')) return make_token(TokenType::PercentAssign, start, start_line, start_column);
        return make_token(TokenType::Percent, start, start_line, start_column);
    case '=':
        if (match('=')) return make_token(TokenType::Equal, start, start_line, start_column);
        return make_token(TokenType::Assign, start, start_line, start_column);
    case '!':
        if (match('=')) return make_token(TokenType::NotEqual, start, start_line, start_column);
        return make_token(TokenType::Not, start, start_line, start_column);
    case '<':
        if (match('<')) {
            if (match('=')) return make_token(TokenType::ShiftLeftAssign, start, start_line, start_column);
            return make_token(TokenType::ShiftLeft, start, start_line, start_column);
        }
        if (match('=')) return make_token(TokenType::LessEqual, start, start_line, start_column);
        return make_token(TokenType::Less, start, start_line, start_column);
    case '>':
        if (match('>')) {
            if (match('=')) return make_token(TokenType::ShiftRightAssign, start, start_line, start_column);
            return make_token(TokenType::ShiftRight, start, start_line, start_column);
        }
        if (match('=')) return make_token(TokenType::GreaterEqual, start, start_line, start_column);
        return make_token(TokenType::Greater, start, start_line, start_column);
    case '&':
        if (match('&')) return make_token(TokenType::AndAnd, start, start_line, start_column);
        if (match('=')) return make_token(TokenType::AndAssign, start, start_line, start_column);
        return make_token(TokenType::And, start, start_line, start_column);
    case '|':
        if (match('|')) return make_token(TokenType::OrOr, start, start_line, start_column);
        if (match('=')) return make_token(TokenType::OrAssign, start, start_line, start_column);
        return make_token(TokenType::Or, start, start_line, start_column);
    case '^':
        if (match('=')) return make_token(TokenType::XorAssign, start, start_line, start_column);
        return make_token(TokenType::Xor, start, start_line, start_column);
    case '~': return make_token(TokenType::Tilde, start, start_line, start_column);
    default:
        return make_token(TokenType::Invalid, start, start_line, start_column);
    }
}

TokenType Lexer::identifier_keyword(const std::string& text) const {
    static const std::unordered_map<std::string, TokenType> kKeywords = {
        {"include", TokenType::KwInclude},
        {"public", TokenType::KwPublic},
        {"private", TokenType::KwPrivate},
        {"int8", TokenType::KwInt8},
        {"int16", TokenType::KwInt16},
        {"int32", TokenType::KwInt32},
        {"int64", TokenType::KwInt64},
        {"uint8", TokenType::KwUint8},
        {"uint16", TokenType::KwUint16},
        {"uint32", TokenType::KwUint32},
        {"uint64", TokenType::KwUint64},
        {"float", TokenType::KwFloat},
        {"double", TokenType::KwDouble},
        {"char", TokenType::KwChar},
        {"void", TokenType::KwVoid},
        {"bool", TokenType::KwBool},
        {"struct", TokenType::KwStruct},
        {"static", TokenType::KwStatic},
        {"if", TokenType::KwIf},
        {"else", TokenType::KwElse},
        {"while", TokenType::KwWhile},
        {"for", TokenType::KwFor},
        {"in", TokenType::KwIn},
        {"match", TokenType::KwMatch},
        {"case", TokenType::KwCase},
        {"default", TokenType::KwDefault},
        {"yield", TokenType::KwYield},
        {"return", TokenType::KwReturn},
        {"fallthrough", TokenType::KwFallthrough},
        {"new", TokenType::KwNew},
        {"delete", TokenType::KwDelete},
        {"template", TokenType::KwTemplate},
        {"typename", TokenType::KwTypename},
        {"this", TokenType::KwThis},
        {"true", TokenType::KwTrue},
        {"false", TokenType::KwFalse},
        {"nullptr", TokenType::KwNullptr},
    };

    const auto it = kKeywords.find(text);
    return it == kKeywords.end() ? TokenType::Identifier : it->second;
}

} // namespace dino::frontend
