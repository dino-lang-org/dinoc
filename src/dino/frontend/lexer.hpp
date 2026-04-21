#pragma once

#include "dino/frontend/token.hpp"

#include <string>
#include <vector>

namespace dino::frontend {

class Lexer {
public:
    Lexer(std::string file, std::string source);

    std::vector<Token> tokenize();

private:
    [[nodiscard]] bool is_at_end() const;
    [[nodiscard]] char peek(size_t offset = 0) const;
    char advance();
    bool match(char expected);

    void skip_whitespace_and_comments();

    [[nodiscard]] Token make_token(TokenType type, size_t start, size_t start_line, size_t start_column) const;
    Token scan_identifier_or_keyword(size_t start, size_t start_line, size_t start_column);
    Token scan_number(size_t start, size_t start_line, size_t start_column);
    Token scan_string(size_t start, size_t start_line, size_t start_column);
    Token scan_character(size_t start, size_t start_line, size_t start_column);
    Token scan_operator_or_punct(size_t start, size_t start_line, size_t start_column);

    [[nodiscard]] TokenType identifier_keyword(const std::string& text) const;

private:
    std::string file_;
    std::string source_;
    size_t pos_ = 0;
    size_t line_ = 1;
    size_t column_ = 1;
};

} // namespace dino::frontend
