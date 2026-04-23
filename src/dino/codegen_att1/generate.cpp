#include "codegen.hpp"

#include "dino/frontend/ast.hpp"
#include "dino/frontend/parser.hpp"

std::vector<std::unique_ptr<llvm::Module>> dino::codegen::LLVMBasedCodeGenerator::generate() {
    auto units_iterator = this->parse_result_->units.begin();
    for (int i = 0; i < this->parse_result_->units.size(); ++i) {
        const auto& [_, translation_unit] = *units_iterator;
        std::printf("Processing codegen for TU '%s' :: \n", translation_unit->file_path.c_str());
        std::printf("\tExported ::\n");
        for (const auto& symbol : translation_unit->exported_symbols)
            std::printf("\t\t%s\n", symbol.c_str());
        std::printf("\tLocal ::\n");
        for (const auto& symbol : translation_unit->local_symbols)
            std::printf("\t\t%s\n", symbol.c_str());
        std::advance(units_iterator, 1);
    }
    return {};
}
