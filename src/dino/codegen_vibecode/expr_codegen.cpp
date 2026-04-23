#include "dino/codegen_vibecode/expr_codegen.hpp"
#include "dino/codegen_vibecode/type_converter.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Type.h>

namespace dino::codegen {
    ExprCodeGen::ExprCodeGen(::llvm::IRBuilder<> &builder, TypeConverter &type_converter)
        : builder_(builder), type_converter_(type_converter) {

    }

    ::llvm::Value *ExprCodeGen::generate(const dino::frontend::Expr &expr) {
        if (auto* lit = dynamic_cast<const dino::frontend::LiteralExpr*>(&expr)) {
            return generateLiteral(*lit);
        }
        if (auto* id = dynamic_cast<const dino::frontend::IdentifierExpr*>(&expr)) {
            return generateIdentifier(*id);
        }
        if (auto* unary = dynamic_cast<const dino::frontend::UnaryExpr*>(&expr)) {
            return generateUnary(*unary);
        }
        if (auto* binary = dynamic_cast<const dino::frontend::BinaryExpr*>(&expr)) {
            return generateBinary(*binary);
        }
        if (auto* ternary = dynamic_cast<const dino::frontend::TernaryExpr*>(&expr)) {
            return generateTernary(*ternary);
        }
        if (auto* call = dynamic_cast<const dino::frontend::CallExpr*>(&expr)) {
            return generateCall(*call);
        }
        if (auto* member = dynamic_cast<const dino::frontend::MemberExpr*>(&expr)) {
            return generateMember(*member);
        }
        if (auto* cast = dynamic_cast<const dino::frontend::TypeCastExpr*>(&expr)) {
            return generateTypeCast(*cast);
        }
        if (auto* if_expr = dynamic_cast<const dino::frontend::IfExpr*>(&expr)) {
            return generateIf(*if_expr);
        }
        if (auto* match = dynamic_cast<const dino::frontend::MatchExpr*>(&expr)) {
            return generateMatch(*match);
        }
        if (auto* index = dynamic_cast<const dino::frontend::IndexExpr*>(&expr)) {
            return generateIndex(*index);
        }
        return nullptr;
    }

    ::llvm::Value* ExprCodeGen::generateLiteral(const dino::frontend::LiteralExpr& lit) {
        if (lit.literal_kind == "Number") {
            return ::llvm::ConstantInt::get(builder_.getInt32Ty(), std::stoi(lit.value));
        } else if (lit.literal_kind == "Float") {
            return ::llvm::ConstantFP::get(builder_.getDoubleTy(), std::stod(lit.value));
        } else if (lit.literal_kind == "Bool") {
            bool val = (lit.value == "true");
            return ::llvm::ConstantInt::get(builder_.getInt1Ty(), val ? 1 : 0);
        }
        return nullptr;
    }

    ::llvm::Value* ExprCodeGen::generateIdentifier(const dino::frontend::IdentifierExpr& id) {
        return getNamedValue(id.name);
    }

    ::llvm::Value* ExprCodeGen::generateUnary(const dino::frontend::UnaryExpr& unary) {
        return nullptr;
    }

    ::llvm::Value* ExprCodeGen::generateBinary(const dino::frontend::BinaryExpr& binary) {
        ::llvm::Value* lhs = generate(*binary.lhs);
        ::llvm::Value* rhs = generate(*binary.rhs);
        if (!lhs || !rhs) return nullptr;
        ::llvm::Type* type = lhs->getType();
        if (binary.op == "+") return generateArithmeticOp("+", lhs, rhs, type);
        else if (binary.op == "-") return generateArithmeticOp("-", lhs, rhs, type);
        else if (binary.op == "*") return generateArithmeticOp("*", lhs, rhs, type);
        else if (binary.op == "/") return generateArithmeticOp("/", lhs, rhs, type);
        return nullptr;
    }

    ::llvm::Value* ExprCodeGen::generateTernary(const dino::frontend::TernaryExpr& ternary) {
        return nullptr;
    }

    ::llvm::Value* ExprCodeGen::generateCall(const dino::frontend::CallExpr& call) {
        return nullptr;
    }

    ::llvm::Value* ExprCodeGen::generateMember(const dino::frontend::MemberExpr& member) {
        return nullptr;
    }

    ::llvm::Value* ExprCodeGen::generateTypeCast(const dino::frontend::TypeCastExpr& cast) {
        return nullptr;
    }

    ::llvm::Value* ExprCodeGen::generateIf(const dino::frontend::IfExpr& if_expr) {
        return nullptr;
    }

    ::llvm::Value* ExprCodeGen::generateMatch(const dino::frontend::MatchExpr& match) {
        return nullptr;
    }

    ::llvm::Value *ExprCodeGen::generateArithmeticOp(const std::string &op, ::llvm::Value *lhs, ::llvm::Value *rhs, ::llvm::Type *type) {
        if (type->isIntegerTy()) {
            if (op == "+") return builder_.CreateAdd(lhs, rhs);
            else if (op == "-") return builder_.CreateSub(lhs, rhs);
            else if (op == "*") return builder_.CreateMul(lhs, rhs);
            else if (op == "/") return builder_.CreateSDiv(lhs, rhs);
        } else if (type->isFloatingPointTy()) {
            if (op == "+") return builder_.CreateFAdd(lhs, rhs);
            else if (op == "-") return builder_.CreateFSub(lhs, rhs);
            else if (op == "*") return builder_.CreateFMul(lhs, rhs);
            else if (op == "/") return builder_.CreateFDiv(lhs, rhs);
        }
        return nullptr;
    }

    ::llvm::Value *ExprCodeGen::generateComparisonOp(const std::string &op, ::llvm::Value *lhs, ::llvm::Value *rhs, ::llvm::Type *type) {
        return nullptr;
    }

    ::llvm::Value *ExprCodeGen::generateLogicalOp(const std::string &op, ::llvm::Value *lhs, ::llvm::Value *rhs) {
        return nullptr;
    }

    ::llvm::Value* ExprCodeGen::generateIndex(const dino::frontend::IndexExpr& index) {
        return nullptr;
    }
} // namespace dino::codegen
