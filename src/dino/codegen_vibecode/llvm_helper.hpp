#pragma once

#include "dino/frontend/token.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <unordered_map>

namespace dino {
namespace frontend {
struct TypeRef;
struct Parameter;
} // namespace frontend
} // namespace dino

namespace dino::codegen {

using LLVMBasicBlock = llvm::BasicBlock;
using LLVMValue = llvm::Value;
using LLVMType = llvm::Type;
using LLVMFunction = llvm::Function;

// Helper functions for LLVM IR generation
namespace helpers {

// Get LLVM type for a Dino TypeRef
llvm::Type* getLLVMType(llvm::LLVMContext& context, const dino::frontend::TypeRef& type);

// Create a new basic block
LLVMBasicBlock* createBlock(llvm::LLVMContext& context, const std::string& name,
                            llvm::Function* function = nullptr);

// Create a constant value
llvm::Constant* createConstant(llvm::LLVMContext& context,
                              const dino::frontend::LiteralExpr& literal);

} // namespace helpers

} // namespace dino::codegen
