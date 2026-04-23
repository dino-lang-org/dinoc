#include "codegen.hpp"
#include "dino/frontend/parser.hpp"

llvm::StructType *dino::codegen::LLVMBasedCodeGenerator::found_struct_declaration_in_any_tu(const std::string &struct_name, int current_i) {
    if (this->structures_.contains(struct_name)) return this->structures_[struct_name];
    auto iterator = this->parse_result_->units.begin();
    std::advance(iterator, current_i);
    for (int i = current_i; i < this->parse_result_->units.size(); ++i) {
        const auto& [_, tu] = *iterator;
        if (tu->)
    }
}
