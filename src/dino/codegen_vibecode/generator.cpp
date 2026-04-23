// Code generation implementation
#include "dino/codegen_vibecode/generator.hpp"
#include "dino/codegen_vibecode/type_converter.hpp"
#include "dino/codegen_vibecode/expr_codegen.hpp"
#include "dino/codegen_vibecode/stmt_codegen.hpp"
#include "dino/codegen_vibecode/decl_codegen.hpp"
#include "dino/frontend/sema.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/TargetSelect.h>

namespace dino::codegen {

LLVMGenerator::LLVMGenerator()
    : context_(std::make_unique<::llvm::LLVMContext>()),
      builder_(std::make_unique<::llvm::IRBuilder<>>(*context_)),
      type_converter_(std::make_unique<TypeConverter>(*context_)) {
}

LLVMGenerator::~LLVMGenerator() = default;

bool LLVMGenerator::generate(
    const std::unordered_map<std::string, std::unique_ptr<dino::frontend::TranslationUnit>>& units,
    const dino::frontend::TypeCheckResult& type_result) {
    (void)type_result;
    
    if (!initializeTarget()) {
        return false;
    }

    module_ = std::make_unique<::llvm::Module>("dino", *context_);
    type_converter_->module_ = module_.get();

    ExprCodeGen expr_gen(*builder_, *type_converter_);
    StmtCodeGen stmt_gen(*builder_, *type_converter_, expr_gen);
    DeclCodeGen decl_gen(*builder_, *type_converter_, expr_gen, stmt_gen);

    for (const auto& [path, unit] : units) {
        decl_gen.generateTranslationUnit(*unit);
    }

    return true;
}

std::string LLVMGenerator::getIR() const {
    if (!module_) return "";
    std::string result;
    ::llvm::raw_string_ostream stream(result);
    module_->print(stream, nullptr);
    stream.flush();
    return result;
}

bool LLVMGenerator::emitObject(const std::string& output_path) const {
    (void)output_path;
    return module_ != nullptr;
}

bool LLVMGenerator::initializeTarget() {
    ::llvm::InitializeAllTargetInfos();
    ::llvm::InitializeAllTargets();
    ::llvm::InitializeAllTargetMCs();
    ::llvm::InitializeAllAsmParsers();
    ::llvm::InitializeAllAsmPrinters();
    return true;
}

} // namespace dino::codegen
