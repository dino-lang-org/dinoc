#pragma once

#include "dino/frontend/ast.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <unordered_map>

namespace dino::codegen {

// Forward declare LLVM types
namespace llvm {
class Type;
class StructType;
}

class TypeConverter {
public:
    TypeConverter(::llvm::LLVMContext& context);

    // Convert a Dino TypeRef to LLVM Type
    ::llvm::Type* convertType(const dino::frontend::TypeRef& type);

    // Convert a Dino function type to LLVM FunctionType
    ::llvm::FunctionType* convertFunctionType(const dino::frontend::TypeRef& return_type,
                                           const std::vector<dino::frontend::Parameter>& params);

    // Register a struct type (forward declaration)
    ::llvm::StructType* registerStruct(const std::string& name);

    // Complete a struct type definition
    void completeStruct(::llvm::StructType* struct_type,
                       const std::vector<dino::frontend::FieldDecl>& fields);

    // Get a registered struct type
    ::llvm::StructType* getStruct(const std::string& name);

private:
    ::llvm::LLVMContext& context_;
    ::llvm::Module* module_;  // Non-owning pointer to module for struct types
    std::unordered_map<std::string, ::llvm::StructType*> structs_;

    // Convert primitive types
    ::llvm::Type* convertPrimitive(const std::string& type_name);

    friend class LLVMGenerator;
};

} // namespace dino::codegen
