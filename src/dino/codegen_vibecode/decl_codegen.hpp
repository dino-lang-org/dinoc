#pragma once

#include <llvm/IR/IRBuilder.h>

#include "dino/frontend/ast.hpp"

namespace dino::codegen {

class TypeConverter;
class ExprCodeGen;
class StmtCodeGen;

class DeclCodeGen {
public:
    DeclCodeGen(::llvm::IRBuilder<>& builder, TypeConverter& type_converter,
                ExprCodeGen& expr_gen, StmtCodeGen& stmt_gen);

    // Generate LLVM IR for a declaration
    void generate(const dino::frontend::Decl& decl);

    // Generate all declarations in a translation unit
    void generateTranslationUnit(const dino::frontend::TranslationUnit& unit);

private:
    ::llvm::IRBuilder<>& builder_;
    TypeConverter& type_converter_;
    ExprCodeGen& expr_gen_;
    StmtCodeGen& stmt_gen_;

    // Declaration generators
    void generateStruct(const dino::frontend::StructDecl& struct_decl);
    void generateFunction(const dino::frontend::FunctionDecl& func_decl);
    void generateInclude(const dino::frontend::IncludeDecl& include_decl);

    // Generate struct member functions
    void generateMethod(const dino::frontend::MethodDecl& method, const std::string& struct_name);
    void generateConstructor(const dino::frontend::ConstructorDecl& ctor, const std::string& struct_name);
    void generateDestructor(const dino::frontend::DestructorDecl& dtor, const std::string& struct_name);
    void generateConversion(const dino::frontend::ConversionDecl& conv, const std::string& struct_name);

    // Track generated structs
    std::unordered_map<std::string, ::llvm::StructType*> struct_types_;

public:
    ::llvm::StructType* getStructType(const std::string& name) const {
        auto it = struct_types_.find(name);
        if (it != struct_types_.end()) {
            return it->second;
        }
        return nullptr;
    }
};

} // namespace dino::codegen
