#include "dino/codegen_vibecode/decl_codegen.hpp"
#include "dino/codegen_vibecode/expr_codegen.hpp"
#include "dino/codegen_vibecode/stmt_codegen.hpp"
#include "dino/codegen_vibecode/type_converter.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>

namespace dino::codegen {

DeclCodeGen::DeclCodeGen(::llvm::IRBuilder<>& builder, TypeConverter& type_converter,
                        ExprCodeGen& expr_gen, StmtCodeGen& stmt_gen)
    : builder_(builder), type_converter_(type_converter),
      expr_gen_(expr_gen), stmt_gen_(stmt_gen) {
}

void DeclCodeGen::generate(const dino::frontend::Decl& decl) {
    const dino::frontend::Decl* decl_ptr = &decl;
    if (auto* struct_decl = dynamic_cast<const dino::frontend::StructDecl*>(decl_ptr)) {
        generateStruct(*struct_decl);
    } else if (auto* func_decl = dynamic_cast<const dino::frontend::FunctionDecl*>(decl_ptr)) {
        generateFunction(*func_decl);
    } else if (auto* include_decl = dynamic_cast<const dino::frontend::IncludeDecl*>(decl_ptr)) {
        generateInclude(*include_decl);
    }
}

void DeclCodeGen::generateTranslationUnit(const dino::frontend::TranslationUnit& unit) {
    // Generate all declarations in the translation unit
    for (const auto& decl : unit.declarations) {
        if (decl) {
            generate(*decl);
        }
    }
}

void DeclCodeGen::generateStruct(const dino::frontend::StructDecl& struct_decl) {
    // Create a new struct type
    std::vector<::llvm::Type*> field_types;
    
    // Collect field types
    for (const auto& field : struct_decl.fields) {
        // Convert Dino type to LLVM type
        ::llvm::Type* llvm_type = type_converter_.convertType(field.type);
        if (llvm_type) {
            // Add the type for each field name
            for (size_t i = 0; i < field.names.size(); ++i) {
                field_types.push_back(llvm_type);
            }
        }
    }
    
    // Create the struct type
    ::llvm::StructType* llvm_struct = ::llvm::StructType::create(builder_.getContext(), field_types, struct_decl.name);
    
    // Store the struct type for later use
    struct_types_[struct_decl.name] = llvm_struct;
    
    // Generate member functions
    for (const auto& method : struct_decl.methods) {
        generateMethod(method, struct_decl.name);
    }
    
    // Generate constructors
    for (const auto& ctor : struct_decl.constructors) {
        generateConstructor(ctor, struct_decl.name);
    }
    
    // Generate destructors
    for (const auto& dtor : struct_decl.destructors) {
        generateDestructor(dtor, struct_decl.name);
    }
    
    // Generate conversions
    for (const auto& conv : struct_decl.conversions) {
        generateConversion(conv, struct_decl.name);
    }
}

void DeclCodeGen::generateFunction(const dino::frontend::FunctionDecl& func_decl) {
    // Convert return type to LLVM type
    ::llvm::Type* return_type = type_converter_.convertType(func_decl.return_type);
    if (!return_type) {
        return; // Error in type conversion
    }
    
    // Convert parameter types to LLVM types
    std::vector<::llvm::Type*> param_types;
    for (const auto& param : func_decl.parameters) {
        ::llvm::Type* param_type = type_converter_.convertType(param.type);
        if (param_type) {
            param_types.push_back(param_type);
        }
    }
    
    // Create function type
    ::llvm::FunctionType* func_type = ::llvm::FunctionType::get(return_type, param_types, false);
    
    // Create function
    ::llvm::Function* func = ::llvm::Function::Create(func_type, 
                                                     ::llvm::Function::ExternalLinkage, 
                                                     func_decl.name, 
                                                     builder_.GetInsertBlock()->getModule());
    
    // Set parameter names
    unsigned idx = 0;
    for (auto& arg : func->args()) {
        arg.setName(func_decl.parameters[idx].name);
        idx++;
    }
    
    // If the function has a body, generate it
    if (func_decl.body) {
        // Create a new basic block for the function
        ::llvm::BasicBlock* bb = ::llvm::BasicBlock::Create(builder_.getContext(), "entry", func);
        builder_.SetInsertPoint(bb);
        
        // Add function parameters to the named values map
        idx = 0;
        for (auto& arg : func->args()) {
            expr_gen_.setNamedValue(func_decl.parameters[idx].name, &arg);
            idx++;
        }
        
        // Generate the function body
        stmt_gen_.setCurrentFunction(func);
        stmt_gen_.generateBlock(*func_decl.body);
    }
}

void DeclCodeGen::generateInclude(const dino::frontend::IncludeDecl& include_decl) {
    // For now, just return as a placeholder
    return;
}

void DeclCodeGen::generateMethod(const dino::frontend::MethodDecl& method,
                                const std::string& struct_name) {
    // Convert return type to LLVM type
    ::llvm::Type* return_type = type_converter_.convertType(method.return_type);
    if (!return_type) {
        return; // Error in type conversion
    }
    
    // Convert parameter types to LLVM types
    std::vector<::llvm::Type*> param_types;
    // Add implicit 'this' parameter for methods
    param_types.push_back(type_converter_.getStruct(struct_name)); // 'this' parameter
    for (const auto& param : method.parameters) {
        ::llvm::Type* param_type = type_converter_.convertType(param.type);
        if (param_type) {
            param_types.push_back(param_type);
        }
    }
    
    // Create function type
    ::llvm::FunctionType* func_type = ::llvm::FunctionType::get(return_type, param_types, false);
    
    // Create function name (method name is prefixed with struct name)
    std::string func_name = struct_name + "::" + method.name;
    
    // Create function
    ::llvm::Function* func = ::llvm::Function::Create(func_type, 
                                                     ::llvm::Function::ExternalLinkage, 
                                                     func_name, 
                                                     builder_.GetInsertBlock()->getModule());
    
    // If the method has a body, generate it
    if (method.body) {
        // Create a new basic block for the function
        ::llvm::BasicBlock* bb = ::llvm::BasicBlock::Create(builder_.getContext(), "entry", func);
        builder_.SetInsertPoint(bb);
        
        // Add function parameters to the named values map
        unsigned idx = 0;
        for (auto& arg : func->args()) {
            if (idx == 0) {
                // First parameter is 'this'
                expr_gen_.setNamedValue("this", &arg);
            } else {
                expr_gen_.setNamedValue(method.parameters[idx-1].name, &arg);
            }
            idx++;
        }
        
        // Generate the function body
        stmt_gen_.setCurrentFunction(func);
        stmt_gen_.generateBlock(*method.body);
    }
}

void DeclCodeGen::generateConstructor(const dino::frontend::ConstructorDecl& ctor,
                                   const std::string& struct_name) {
    // Convert parameter types to LLVM types
    std::vector<::llvm::Type*> param_types;
    // Add implicit 'this' parameter for constructors
    if (type_converter_.getStruct(struct_name)) {
        param_types.push_back(type_converter_.getStruct(struct_name)->getPointerTo()); // 'this' parameter
    }
    
    for (const auto& param : ctor.parameters) {
        ::llvm::Type* param_type = type_converter_.convertType(param.type);
        if (param_type) {
            param_types.push_back(param_type);
        }
    }
    
    // Create function type
    ::llvm::FunctionType* func_type = ::llvm::FunctionType::get(builder_.getVoidTy(), param_types, false);
    
    // Create function name (constructor name is prefixed with struct name)
    std::string func_name = struct_name + "::" + ctor.name;
    
    // Create function
    ::llvm::Function* func = ::llvm::Function::Create(func_type, 
                                                     ::llvm::Function::ExternalLinkage, 
                                                     func_name, 
                                                     builder_.GetInsertBlock()->getModule());
    
    // If the constructor has a body, generate it
    if (ctor.body) {
        // Create a new basic block for the function
        ::llvm::BasicBlock* bb = ::llvm::BasicBlock::Create(builder_.getContext(), "entry", func);
        builder_.SetInsertPoint(bb);
        
        // Generate the function body
        stmt_gen_.setCurrentFunction(func);
        stmt_gen_.generateBlock(*ctor.body);
    }
}

void DeclCodeGen::generateDestructor(const dino::frontend::DestructorDecl& dtor,
                                  const std::string& struct_name) {
    // Create function type (no parameters, void return type)
    ::llvm::FunctionType* func_type = ::llvm::FunctionType::get(builder_.getVoidTy(), false);
    
    // Create function name (destructor name is prefixed with struct name)
    std::string func_name = struct_name + "::" + dtor.name;
    
    // Create function
    ::llvm::Function* func = ::llvm::Function::Create(func_type, 
                                                     ::llvm::Function::ExternalLinkage, 
                                                     func_name, 
                                                     builder_.GetInsertBlock()->getModule());
    
    // If the destructor has a body, generate it
    if (dtor.body) {
        // Create a new basic block for the function
        ::llvm::BasicBlock* bb = ::llvm::BasicBlock::Create(builder_.getContext(), "entry", func);
        builder_.SetInsertPoint(bb);
        
        // Generate the function body
        stmt_gen_.setCurrentFunction(func);
        stmt_gen_.generateBlock(*dtor.body);
    }
}

void DeclCodeGen::generateConversion(const dino::frontend::ConversionDecl& conv,
                                  const std::string& struct_name) {
    // Convert return type to LLVM type
    ::llvm::Type* return_type = type_converter_.convertType(conv.target_type);
    if (!return_type) {
        return; // Error in type conversion
    }
    
    // Create function type (conversion functions typically take no parameters)
    ::llvm::FunctionType* func_type = ::llvm::FunctionType::get(return_type, false);
    
    // Create function name (conversion name is prefixed with struct name)
    std::string func_name = struct_name + "::operator " + conv.target_type.name;
    
    // Create function
    ::llvm::Function* func = ::llvm::Function::Create(func_type, 
                                                     ::llvm::Function::ExternalLinkage, 
                                                     func_name, 
                                                     builder_.GetInsertBlock()->getModule());
    
    // If the conversion has a body, generate it
    if (conv.body) {
        // Create a new basic block for the function
        ::llvm::BasicBlock* bb = ::llvm::BasicBlock::Create(builder_.getContext(), "entry", func);
        builder_.SetInsertPoint(bb);
        
        // Generate the function body
        stmt_gen_.setCurrentFunction(func);
        stmt_gen_.generateBlock(*conv.body);
    }
}

} // namespace dino::codegen
