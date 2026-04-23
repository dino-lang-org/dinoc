#pragma once

#include "dino/frontend/ast.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>

namespace dino::codegen {
inline namespace llvm_alias {
using ::llvm::IRBuilder;
using ::llvm::Value;
using ::llvm::Type;
}

class TypeConverter;

class ExprCodeGen {
public:
    ExprCodeGen(::llvm::IRBuilder<>& builder, TypeConverter& type_converter);

    // Generate LLVM IR for an expression
    ::llvm::Value* generate(const dino::frontend::Expr& expr);

    // Generate assignment (for lvalues)
    ::llvm::Value* generateAssignment(::llvm::Value* lhs, ::llvm::Value* rhs,
                                   const dino::frontend::TypeRef& type);

private:
    ::llvm::IRBuilder<>& builder_;
    TypeConverter& type_converter_;

    // Expression generators
    ::llvm::Value* generateLiteral(const dino::frontend::LiteralExpr& lit);
    ::llvm::Value* generateIdentifier(const dino::frontend::IdentifierExpr& id);
    ::llvm::Value* generateUnary(const dino::frontend::UnaryExpr& unary);
    ::llvm::Value* generateBinary(const dino::frontend::BinaryExpr& binary);
    ::llvm::Value* generateTernary(const dino::frontend::TernaryExpr& ternary);
    ::llvm::Value* generateCall(const dino::frontend::CallExpr& call);
    ::llvm::Value* generateMember(const dino::frontend::MemberExpr& member);
    ::llvm::Value* generateTypeCast(const dino::frontend::TypeCastExpr& cast);
    ::llvm::Value* generateIf(const dino::frontend::IfExpr& if_expr);
    ::llvm::Value* generateMatch(const dino::frontend::MatchExpr& match);

    // Helper for binary operations
    ::llvm::Value* generateArithmeticOp(const std::string& op,
                                      ::llvm::Value* lhs, ::llvm::Value* rhs,
                                      ::llvm::Type* type);
    ::llvm::Value* generateComparisonOp(const std::string& op,
                                      ::llvm::Value* lhs, ::llvm::Value* rhs,
                                      ::llvm::Type* type);
    ::llvm::Value* generateLogicalOp(const std::string& op,
                                 ::llvm::Value* lhs, ::llvm::Value* rhs);

    // For array indexing
    ::llvm::Value* generateIndex(const dino::frontend::IndexExpr& index);

    // Value tracking (for variables in scope)
    std::unordered_map<std::string, ::llvm::Value*> named_values_;

public:
    void setNamedValue(const std::string& name, ::llvm::Value* value) {
        named_values_[name] = value;
    }

    ::llvm::Value* getNamedValue(const std::string& name) const {
        auto it = named_values_.find(name);
        if (it != named_values_.end()) {
            return it->second;
        }
        return nullptr;
    }
};

} // namespace dino::codegen
