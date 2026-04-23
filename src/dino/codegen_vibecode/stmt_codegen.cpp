#include "dino/codegen_vibecode/stmt_codegen.hpp"
#include "dino/codegen_vibecode/expr_codegen.hpp"
#include "dino/codegen_vibecode/type_converter.hpp"

namespace dino::codegen {

StmtCodeGen::StmtCodeGen(::llvm::IRBuilder<>& builder, TypeConverter& type_converter,
                        ExprCodeGen& expr_gen)
    : builder_(builder), type_converter_(type_converter), expr_gen_(expr_gen) {
}

void StmtCodeGen::generateBlock(const dino::frontend::BlockStmt& block) {
    for (const auto& stmt : block.statements) {
        if (stmt) {
            generate(*stmt);
        }
    }
}

void StmtCodeGen::generate(const dino::frontend::Stmt& stmt) {
    // Dispatch to appropriate generator based on statement type
    if (auto* return_stmt = dynamic_cast<const dino::frontend::ReturnStmt*>(&stmt)) {
        generateReturn(*return_stmt);
    } else if (auto* if_stmt = dynamic_cast<const dino::frontend::IfStmt*>(&stmt)) {
        generateIf(*if_stmt);
    } else if (auto* while_stmt = dynamic_cast<const dino::frontend::WhileStmt*>(&stmt)) {
        generateWhile(*while_stmt);
    } else if (auto* for_stmt = dynamic_cast<const dino::frontend::ForStmt*>(&stmt)) {
        generateFor(*for_stmt);
    } else if (auto* expr_stmt = dynamic_cast<const dino::frontend::ExprStmt*>(&stmt)) {
        generateExprStmt(*expr_stmt);
    } else if (auto* var_decl = dynamic_cast<const dino::frontend::VarDeclStmt*>(&stmt)) {
        generateVarDecl(*var_decl);
    }
    // Note: Yield and Fallthrough statements are not implemented yet
}

void StmtCodeGen::generateReturn(const dino::frontend::ReturnStmt& ret) {
    // Generate the return value if it exists
    ::llvm::Value* return_value = nullptr;
    if (ret.value) {
        return_value = expr_gen_.generate(*ret.value);
    }
    
    // Create the return instruction
    if (return_value) {
        builder_.CreateRet(return_value);
    } else {
        builder_.CreateRetVoid();
    }
}

void StmtCodeGen::generateIf(const dino::frontend::IfStmt& if_stmt) {
    // Generate the condition
    ::llvm::Value* condition_value = nullptr;
    if (if_stmt.condition) {
        condition_value = expr_gen_.generate(*if_stmt.condition);
    }
    
    // Get the current function
    ::llvm::Function* func = builder_.GetInsertBlock()->getParent();
    
    // Create basic blocks for then, else, and merge
    ::llvm::BasicBlock* then_bb = ::llvm::BasicBlock::Create(builder_.getContext(), "then", func);
    ::llvm::BasicBlock* else_bb = nullptr;
    if (if_stmt.else_stmt) {
        else_bb = ::llvm::BasicBlock::Create(builder_.getContext(), "else", func);
    }
    ::llvm::BasicBlock* merge_bb = ::llvm::BasicBlock::Create(builder_.getContext(), "ifcont", func);
    
    // Create the conditional branch
    if (else_bb) {
        builder_.CreateCondBr(condition_value, then_bb, else_bb);
    } else {
        builder_.CreateCondBr(condition_value, then_bb, merge_bb);
    }
    
    // Generate then block
    builder_.SetInsertPoint(then_bb);
    if (if_stmt.then_stmt) {
        // Generate the then statement
        // For now, we'll just generate the statement without specific handling
        // In a more complete implementation, we would generate the specific statement
    }
    
    // If there's an else statement, jump to the else block
    if (else_bb) {
        builder_.CreateBr(merge_bb);
    }
    
    // If there's an else statement, generate the else block
    if (if_stmt.else_stmt && else_bb) {
        builder_.SetInsertPoint(else_bb);
        // Generate the else statement
        // For now, we'll just generate the statement without specific handling
        builder_.CreateBr(merge_bb);
    }
    
    // Set the insert point to the merge block
    builder_.SetInsertPoint(merge_bb);
}

void StmtCodeGen::generateWhile(const dino::frontend::WhileStmt& while_stmt) {
    // Get the current function
    ::llvm::Function* func = builder_.GetInsertBlock()->getParent();
    
    // Create basic blocks for condition, body, and exit
    ::llvm::BasicBlock* cond_bb = ::llvm::BasicBlock::Create(builder_.getContext(), "cond", func);
    ::llvm::BasicBlock* body_bb = ::llvm::BasicBlock::Create(builder_.getContext(), "body", func);
    ::llvm::BasicBlock* exit_bb = ::llvm::BasicBlock::Create(builder_.getContext(), "exit", func);
    
    // Create the conditional branch for the loop
    builder_.CreateBr(cond_bb);
    
    // Generate condition block
    builder_.SetInsertPoint(cond_bb);
    ::llvm::Value* condition_value = nullptr;
    if (while_stmt.condition) {
        condition_value = expr_gen_.generate(*while_stmt.condition);
    }
    builder_.CreateCondBr(condition_value, body_bb, exit_bb);
    
    // Generate body block
    builder_.SetInsertPoint(body_bb);
    if (while_stmt.body) {
        // Generate the body statement
        // For now, we'll just generate the statement without specific handling
    }
    
    // Jump back to condition
    builder_.CreateBr(cond_bb);
    
    // Set the insert point to the exit block
    builder_.SetInsertPoint(exit_bb);
}

void StmtCodeGen::generateFor(const dino::frontend::ForStmt& for_stmt) {
    // For now, just return as a placeholder
    return;
}

void StmtCodeGen::generateExprStmt(const dino::frontend::ExprStmt& expr_stmt) {
    // Generate the expression in the statement
    if (expr_stmt.expr) {
        expr_gen_.generate(*expr_stmt.expr);
    }
    // For now, just return as a placeholder
    return;
}

void StmtCodeGen::generateVarDecl(const dino::frontend::VarDeclStmt& var_decl) {
    // Convert the variable's type to an LLVM type
    ::llvm::Type* llvm_type = type_converter_.convertType(var_decl.type);
    
    if (!llvm_type) {
        return;
    }
    
    // Create a stack allocation for the variable
    ::llvm::AllocaInst* alloca = builder_.CreateAlloca(llvm_type, nullptr, var_decl.name);
    
    // Generate the initializer if it exists
    ::llvm::Value* init_value = nullptr;
    if (var_decl.init) {
        init_value = expr_gen_.generate(*var_decl.init);
    }
    
    // Store the initial value if it exists
    if (init_value) {
        builder_.CreateStore(init_value, alloca);
    }
    
    // Add the variable to the named values map so it can be referenced later
    expr_gen_.setNamedValue(var_decl.name, alloca);
}

void StmtCodeGen::generateYield(const dino::frontend::YieldStmt& yield) {
    // For now, just return as a placeholder
    return;
}

void StmtCodeGen::generateFallthrough(const dino::frontend::FallthroughStmt& fallthrough) {
    // For now, just return as a placeholder
    return;
}

} // namespace dino::codegen
