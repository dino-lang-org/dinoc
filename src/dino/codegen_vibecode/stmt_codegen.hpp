#pragma once

#include "dino/frontend/ast.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

namespace dino::frontend {
struct TranslationUnit;
}

namespace dino::codegen {

class TypeConverter;
class ExprCodeGen;

class StmtCodeGen {
public:
    StmtCodeGen(::llvm::IRBuilder<>& builder, TypeConverter& type_converter,
                ExprCodeGen& expr_gen);

    // Generate LLVM IR for statements in a block
    void generateBlock(const dino::frontend::BlockStmt& block);

    // Generate LLVM IR for a single statement
    void generate(const dino::frontend::Stmt& stmt);

    // Get the current function being generated
    ::llvm::Function* getCurrentFunction() const { return current_function_; }
    void setCurrentFunction(::llvm::Function* func) { current_function_ = func; }

private:
    ::llvm::IRBuilder<>& builder_;
    TypeConverter& type_converter_;
    ExprCodeGen& expr_gen_;
    ::llvm::Function* current_function_ = nullptr;
    ::llvm::BasicBlock* current_block_ = nullptr;

    // Statement generators
    void generateReturn(const dino::frontend::ReturnStmt& ret);
    void generateIf(const dino::frontend::IfStmt& if_stmt);
    void generateWhile(const dino::frontend::WhileStmt& while_stmt);
    void generateFor(const dino::frontend::ForStmt& for_stmt);
    void generateExprStmt(const dino::frontend::ExprStmt& expr_stmt);
    void generateVarDecl(const dino::frontend::VarDeclStmt& var_decl);
    void generateYield(const dino::frontend::YieldStmt& yield);
    void generateFallthrough(const dino::frontend::FallthroughStmt& fallthrough);
};

} // namespace dino::codegen
