#include "dino/codegen_vibecode/type_converter.hpp"
#include "dino/frontend/ast.hpp"

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

namespace dino::codegen {
    TypeConverter::TypeConverter(::llvm::LLVMContext &context) : context_(context) {

    }

    ::llvm::Type* TypeConverter::convertType(const dino::frontend::TypeRef& type) {
    ::llvm::Type* llvm_type = nullptr;

    // Handle primitive types
    if (type.name == "int8") {
        llvm_type = ::llvm::Type::getInt8Ty(context_);
    } else if (type.name == "int16") {
        llvm_type = ::llvm::Type::getInt16Ty(context_);
    } else if (type.name == "int32") {
        llvm_type = ::llvm::Type::getInt32Ty(context_);
    } else if (type.name == "int64") {
        llvm_type = ::llvm::Type::getInt64Ty(context_);
    } else if (type.name == "uint8") {
        llvm_type = ::llvm::Type::getInt8Ty(context_);
    } else if (type.name == "uint16") {
        llvm_type = ::llvm::Type::getInt16Ty(context_);
    } else if (type.name == "uint32") {
        llvm_type = ::llvm::Type::getInt32Ty(context_);
    } else if (type.name == "uint64") {
        llvm_type = ::llvm::Type::getInt64Ty(context_);
    } else if (type.name == "float") {
        llvm_type = ::llvm::Type::getFloatTy(context_);
    } else if (type.name == "double") {
        llvm_type = ::llvm::Type::getDoubleTy(context_);
    } else if (type.name == "bool") {
        llvm_type = ::llvm::Type::getInt1Ty(context_);
    } else if (type.name == "void") {
        llvm_type = ::llvm::Type::getVoidTy(context_);
    } else if (type.name == "int32") {
        // Default int type
        llvm_type = ::llvm::Type::getInt32Ty(context_);
    } else {
        // Handle structs (forward declared)
        llvm_type = getStruct(type.name);
        if (!llvm_type) {
            // Default to i32 for unknown types
            llvm_type = ::llvm::Type::getInt32Ty(context_);
        }
    }

    // Apply pointer/reference qualifiers
    if (type.is_pointer) {
        llvm_type = llvm_type->getPointerTo();
    }
    if (type.is_reference) {
        llvm_type = llvm_type->getPointerTo();  // References as pointers
    }

    return llvm_type;
}

::llvm::FunctionType* TypeConverter::convertFunctionType(
    const dino::frontend::TypeRef& return_type,
    const std::vector<dino::frontend::Parameter>& params) {

    ::llvm::Type* llvm_return_type = convertType(return_type);
    if (return_type.name == "void") {
        llvm_return_type = ::llvm::Type::getVoidTy(context_);
    }

    std::vector<::llvm::Type*> param_types;
    for (const auto& param : params) {
        param_types.push_back(convertType(param.type));
    }

    return ::llvm::FunctionType::get(llvm_return_type, param_types, false);
}

::llvm::StructType* TypeConverter::registerStruct(const std::string& name) {
    auto* struct_type = ::llvm::StructType::create(context_, name);
    structs_[name] = struct_type;
    return struct_type;
}

void TypeConverter::completeStruct(::llvm::StructType* struct_type,
                                  const std::vector<dino::frontend::FieldDecl>& fields) {
    std::vector<::llvm::Type*> field_types;
    for (const auto& field : fields) {
        for (const auto& field_name : field.names) {
            ::llvm::Type* field_type = convertType(field.type);
            field_types.push_back(field_type);
        }
    }

    struct_type->setBody(field_types);
}

::llvm::StructType* TypeConverter::getStruct(const std::string& name) {
    auto it = structs_.find(name);
    if (it != structs_.end()) {
        return it->second;
    }
    return nullptr;
}

} // namespace dino::codegen
