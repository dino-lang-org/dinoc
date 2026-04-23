#pragma once

#include "dino/frontend/driver.hpp"
#include "dino/frontend/sema.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <memory>
#include <unordered_map>

namespace dino::frontend {
struct TranslationUnit;
struct FunctionDecl;
struct StructDecl;
} // namespace dino::frontend

namespace dino::codegen {

class TypeConverter;

class LLVMGenerator {
public:
    LLVMGenerator();
    ~LLVMGenerator();

    // Generate LLVM IR from parsed translation units
    bool generate(const std::unordered_map<std::string, std::unique_ptr<dino::frontend::TranslationUnit>>& units,
                  const dino::frontend::TypeCheckResult& type_result);

    // Get the generated LLVM IR as text
    std::string getIR() const;

    // Emit object file to the given path
    bool emitObject(const std::string& output_path) const;

    // Get the module (for testing)
    llvm::Module* getModule() const { return module_.get(); }

private:
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::unique_ptr<TypeConverter> type_converter_;

    // Track generated functions
    std::unordered_map<std::string, llvm::Function*> functions_;

    // Code generation methods
    bool generateTranslationUnit(const dino::frontend::TranslationUnit& unit);
    bool generateStruct(const dino::frontend::StructDecl& struct_decl);
    bool generateFunction(const dino::frontend::FunctionDecl& func_decl);

    // Helper to initialize target
    bool initializeTarget();
};

} // namespace dino::codegen
