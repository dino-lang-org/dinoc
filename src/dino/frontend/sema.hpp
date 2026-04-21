#pragma once

#include "dino/frontend/parser.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace dino::frontend {

struct TypeCheckResult {
    std::vector<ParseMessage> errors;
    std::vector<ParseMessage> warnings;

    bool ok() const { return errors.empty(); }
};

TypeCheckResult type_check(const std::unordered_map<std::string, std::unique_ptr<TranslationUnit>>& units);

} // namespace dino::frontend
