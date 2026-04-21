#pragma once

#include "dino/frontend/ast.hpp"
#include "dino/frontend/token.hpp"

#include <iosfwd>
#include <string>
#include <vector>

namespace dino::frontend {

void dump_tokens(const std::vector<Token>& tokens, std::ostream& os);
void dump_ast(const TranslationUnit& unit, std::ostream& os);
void dump_all_asts(const std::vector<const TranslationUnit*>& units, std::ostream& os);

std::string describe_type(const TypeRef& type);

} // namespace dino::frontend
