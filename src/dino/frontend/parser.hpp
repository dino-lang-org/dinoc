#pragma once

#include "dino/frontend/ast.hpp"
#include "dino/frontend/token.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace dino::frontend {

struct ParseOptions {
    bool dump_tokens = false;
    bool dump_ast = false;
};

struct ParseMessage {
    SourceLocation location;
    std::string text;
};

struct ParseResult {
    std::unordered_map<std::string, std::unique_ptr<TranslationUnit>> units;
    std::vector<ParseMessage> errors;
    std::vector<ParseMessage> warnings;

    bool ok() const { return errors.empty(); }
};

class ParserDriver {
public:
    ParseResult parse_entry(const std::string& entry_path);

private:
    bool parse_unit_recursive(const std::string& path, ParseResult& result);
};

std::string resolve_include_path(const std::string& include_path, const std::string& current_file);

} // namespace dino::frontend
