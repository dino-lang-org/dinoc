#pragma once

#include <memory>
#include <utility>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Module.h>

namespace dino {
    namespace frontend {
        struct TranslationUnit;
        struct ParseResult;
    }

    namespace codegen {
        class LLVMBasedCodeGenerator {
            inline static std::unique_ptr<llvm::LLVMContext> llvm_context_ = std::make_unique<llvm::LLVMContext>();
            std::unique_ptr<llvm::IRBuilder<>> ir_builder_;
            std::unordered_map<std::string, llvm::StructType*> structures_;
            std::unordered_map<std::string, std::pair<std::string, llvm::Function*>> functions_; // <name, <fromFile, Function*>>
            std::shared_ptr<frontend::ParseResult> parse_result_;
        public:
            LLVMBasedCodeGenerator(std::shared_ptr<frontend::ParseResult> parse_result) : parse_result_(std::move(parse_result)) {}

            llvm::StructType* found_struct_declaration_in_any_tu(const std::string& struct_name, int current_i);
            std::vector<std::unique_ptr<llvm::Module>> generate();
        };
    }
}