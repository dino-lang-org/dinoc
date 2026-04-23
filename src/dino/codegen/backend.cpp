#include "dino/codegen/backend.hpp"

#include "dino/frontend/parser.hpp"

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <llvm/ADT/APFloat.h>
#include <llvm/ADT/APInt.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

namespace dino::codegen {
namespace {

using namespace dino::frontend;

struct SemanticType {
    std::string name;
    bool is_const = false;
    bool is_nonull = false;
    bool is_pointer = false;
    bool is_reference = false;
    bool is_array = false;
    size_t array_size = 0;
    bool is_error = false;

    [[nodiscard]] bool is_void() const { return !is_error && !is_pointer && !is_reference && !is_array && name == "void"; }
};

struct FieldInfo {
    std::string name;
    SemanticType type;
    size_t index = 0;
};

enum class FunctionKind {
    Free,
    Method,
    Constructor,
    Destructor,
    Conversion,
};

struct FunctionInfo {
    FunctionKind kind = FunctionKind::Free;
    std::string owner;
    std::string name;
    std::string llvm_name;
    SemanticType return_type;
    std::vector<SemanticType> params;
    bool variadic = false;
    bool external_only = false;
    const TranslationUnit* owner_unit = nullptr;
    const FunctionDecl* function = nullptr;
    const MethodDecl* method = nullptr;
    const ConstructorDecl* constructor = nullptr;
    const DestructorDecl* destructor = nullptr;
    const ConversionDecl* conversion = nullptr;
    llvm::Function* llvm_function = nullptr;
};

struct StructInfo {
    const StructDecl* decl = nullptr;
    std::vector<FieldInfo> fields;
    std::unordered_map<std::string, size_t> field_indices;
    std::vector<FunctionInfo*> constructors;
    FunctionInfo* destructor = nullptr;
    std::unordered_map<std::string, std::vector<FunctionInfo*>> methods;
    std::unordered_map<std::string, FunctionInfo*> conversions;
    llvm::StructType* llvm_type = nullptr;
};

struct VariableInfo {
    SemanticType type;
    llvm::Value* address = nullptr;
    bool needs_destructor = false;
};

struct Scope {
    std::unordered_map<std::string, VariableInfo> vars;
    std::vector<std::string> destruction_order;
};

struct YieldContext {
    llvm::AllocaInst* slot = nullptr;
    llvm::BasicBlock* merge_block = nullptr;
    SemanticType type;
    size_t scope_depth = 0;
};

SemanticType from_typeref(const TypeRef& type) {
    SemanticType result;
    result.name = type.name;
    result.is_const = type.is_const;
    result.is_nonull = type.is_nonull;
    result.is_pointer = type.is_pointer;
    result.is_reference = type.is_reference;
    return result;
}

std::string type_to_string(const SemanticType& type) {
    std::string out;
    if (type.is_const) {
        out += "const ";
    }
    if (type.is_nonull) {
        out += "nonull ";
    }
    out += type.name;
    if (type.is_pointer) {
        out += "*";
    }
    if (type.is_reference) {
        out += "&";
    }
    if (type.is_array) {
        out += "[]";
    }
    return out;
}

std::string module_alias_from_include(const std::string& include_path) {
    const std::filesystem::path path(include_path);
    if (!path.stem().empty()) {
        return path.stem().string();
    }
    return include_path;
}

bool is_integer_type(const SemanticType& type) {
    if (type.is_error || type.is_pointer || type.is_reference || type.is_array) {
        return false;
    }
    return type.name == "int8" || type.name == "int16" || type.name == "int32" || type.name == "int64" ||
           type.name == "uint8" || type.name == "uint16" || type.name == "uint32" || type.name == "uint64" ||
           type.name == "char" || type.name == "bool";
}

bool is_signed_integer_type(const SemanticType& type) {
    return type.name == "int8" || type.name == "int16" || type.name == "int32" || type.name == "int64";
}

bool is_float_type(const SemanticType& type) {
    return !type.is_error && !type.is_pointer && !type.is_reference && !type.is_array &&
           (type.name == "float" || type.name == "double");
}

bool is_numeric_type(const SemanticType& type) {
    return is_integer_type(type) || is_float_type(type);
}

bool is_bool_like(const SemanticType& type) {
    return type.name == "bool" || is_numeric_type(type) || type.is_pointer;
}

bool same_type(const SemanticType& lhs, const SemanticType& rhs) {
    return lhs.name == rhs.name && lhs.is_pointer == rhs.is_pointer && lhs.is_reference == rhs.is_reference &&
           lhs.is_array == rhs.is_array && lhs.array_size == rhs.array_size;
}

SemanticType numeric_common_type(const SemanticType& lhs, const SemanticType& rhs) {
    if (lhs.name == "double" || rhs.name == "double") {
        return SemanticType {"double"};
    }
    if (lhs.name == "float" || rhs.name == "float") {
        return SemanticType {"float"};
    }
    if (lhs.name == "uint64" || rhs.name == "uint64" || lhs.name == "int64" || rhs.name == "int64") {
        return SemanticType {"int64"};
    }
    return SemanticType {"int32"};
}

bool is_assignable_to(const SemanticType& from, const SemanticType& to) {
    if (same_type(from, to)) {
        return true;
    }
    if (is_numeric_type(from) && is_numeric_type(to)) {
        return true;
    }
    if (from.is_array && to.is_pointer && from.name == to.name) {
        return true;
    }
    return false;
}

class BackendImpl {
public:
    BackendImpl(llvm::LLVMContext& context, llvm::Module& module, std::ostream& err)
        : context_(context), module_(module), builder_(context), err_(err) {}

    bool generate(const std::unordered_map<std::string, std::unique_ptr<TranslationUnit>>& units) {
        units_ = &units;
        initialize_target_info();
        if (!prepare_target_machine()) {
            return false;
        }
        build_symbols();
        if (!errors_.empty()) {
            flush_errors();
            return false;
        }
        declare_structs();
        define_struct_layouts();
        declare_functions();
        if (!errors_.empty()) {
            flush_errors();
            return false;
        }
        define_functions();
        if (!errors_.empty()) {
            flush_errors();
            return false;
        }
        if (llvm::verifyModule(module_, &llvm::errs())) {
            err_ << "LLVM verifier rejected generated module\n";
            return false;
        }
        return true;
    }

    llvm::TargetMachine* target_machine() const { return target_machine_.get(); }

private:
    struct CallResolution {
        FunctionInfo* function = nullptr;
        SemanticType object_type;
        llvm::Value* object_address = nullptr;
    };

    llvm::LLVMContext& context_;
    llvm::Module& module_;
    llvm::IRBuilder<> builder_;
    std::ostream& err_;
    const std::unordered_map<std::string, std::unique_ptr<TranslationUnit>>* units_ = nullptr;
    std::unordered_map<std::string, StructInfo> structs_;
    std::unordered_map<std::string, std::vector<FunctionInfo*>> free_functions_;
    std::vector<std::unique_ptr<FunctionInfo>> owned_functions_;
    std::vector<std::string> errors_;
    std::unique_ptr<llvm::TargetMachine> target_machine_;
    std::vector<Scope> scopes_;
    const TranslationUnit* current_unit_ = nullptr;
    const StructInfo* current_struct_ = nullptr;
    llvm::Value* current_self_ = nullptr;
    llvm::Function* current_function_ = nullptr;
    YieldContext* current_yield_ = nullptr;
    unsigned string_id_ = 0;

    void initialize_target_info() {
        static bool initialized = false;
        if (!initialized) {
            llvm::InitializeAllTargetInfos();
            llvm::InitializeAllTargets();
            llvm::InitializeAllTargetMCs();
            llvm::InitializeAllAsmPrinters();
            initialized = true;
        }
    }

    bool prepare_target_machine() {
        std::string error;
        const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
        const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
        if (target == nullptr) {
            errors_.push_back(std::format("Failed to initialize LLVM target '{}': {}", triple.str(), error));
            return false;
        }
        llvm::TargetOptions options;
        target_machine_.reset(target->createTargetMachine(triple, "generic", "", options, std::nullopt));
        if (!target_machine_) {
            errors_.push_back("Failed to create LLVM target machine");
            return false;
        }
        module_.setTargetTriple(triple);
        module_.setDataLayout(target_machine_->createDataLayout());
        return true;
    }

    void flush_errors() {
        for (const auto& error : errors_) {
            err_ << "codegen error: " << error << "\n";
        }
    }

    void push_scope() { scopes_.emplace_back(); }

    void pop_scope() {
        if (!scopes_.empty()) {
            if (builder_.GetInsertBlock() != nullptr && builder_.GetInsertBlock()->getTerminator() == nullptr) {
                emit_scope_cleanups(scopes_.size() - 1);
            }
            scopes_.pop_back();
        }
    }

    void declare_variable(const std::string& name, VariableInfo variable) {
        if (scopes_.empty()) {
            push_scope();
        }
        scopes_.back().destruction_order.push_back(name);
        scopes_.back().vars[name] = std::move(variable);
    }

    VariableInfo* lookup_variable(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (auto found = it->vars.find(name); found != it->vars.end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    const VariableInfo* lookup_variable(const std::string& name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (auto found = it->vars.find(name); found != it->vars.end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    llvm::Function* ensure_malloc() {
        if (llvm::Function* fn = module_.getFunction("malloc")) {
            return fn;
        }
        llvm::FunctionType* type = llvm::FunctionType::get(llvm::PointerType::get(context_, 0),
                                                           {llvm::Type::getInt64Ty(context_)},
                                                           false);
        return llvm::Function::Create(type, llvm::Function::ExternalLinkage, "malloc", module_);
    }

    llvm::Function* ensure_free() {
        if (llvm::Function* fn = module_.getFunction("free")) {
            return fn;
        }
        llvm::FunctionType* type = llvm::FunctionType::get(llvm::Type::getVoidTy(context_),
                                                           {llvm::PointerType::get(context_, 0)},
                                                           false);
        return llvm::Function::Create(type, llvm::Function::ExternalLinkage, "free", module_);
    }

    bool type_has_destructor(const SemanticType& type) const {
        if (type.is_pointer || type.is_reference || type.is_array) {
            return false;
        }
        const auto found = structs_.find(type.name);
        return found != structs_.end() && found->second.destructor != nullptr;
    }

    void emit_destructor_call(const SemanticType& type, llvm::Value* address) {
        if (address == nullptr || !type_has_destructor(type)) {
            return;
        }
        const StructInfo& info = structs_.at(type.name);
        builder_.CreateCall(info.destructor->llvm_function, {address});
    }

    void emit_scope_cleanups(size_t scope_index) {
        if (scope_index >= scopes_.size()) {
            return;
        }
        Scope& scope = scopes_[scope_index];
        for (auto it = scope.destruction_order.rbegin(); it != scope.destruction_order.rend(); ++it) {
            const auto found = scope.vars.find(*it);
            if (found == scope.vars.end()) {
                continue;
            }
            if (found->second.needs_destructor) {
                emit_destructor_call(found->second.type, found->second.address);
            }
        }
    }

    void emit_all_scope_cleanups() {
        for (size_t i = scopes_.size(); i > 0; --i) {
            emit_scope_cleanups(i - 1);
        }
    }

    void emit_scope_cleanups_until(size_t target_depth) {
        for (size_t i = scopes_.size(); i > target_depth; --i) {
            emit_scope_cleanups(i - 1);
        }
    }

    void build_symbols() {
        for (const auto& [path, unit] : *units_) {
            (void)path;
            for (const auto& decl : unit->declarations) {
                if (const auto* struct_decl = dynamic_cast<const StructDecl*>(decl.get())) {
                    StructInfo info;
                    info.decl = struct_decl;
                    size_t field_index = 0;
                    for (const auto& field : struct_decl->fields) {
                        for (const auto& name : field.names) {
                            FieldInfo field_info;
                            field_info.name = name;
                            field_info.type = from_typeref(field.type);
                            field_info.index = field_index++;
                            info.field_indices[name] = info.fields.size();
                            info.fields.push_back(field_info);
                        }
                    }
                    structs_[struct_decl->name] = std::move(info);
                }
            }
        }

        for (const auto& [path, unit] : *units_) {
            (void)path;
            for (const auto& decl : unit->declarations) {
                if (const auto* function_decl = dynamic_cast<const FunctionDecl*>(decl.get())) {
                    auto info = std::make_unique<FunctionInfo>();
                    info->kind = FunctionKind::Free;
                    info->name = function_decl->name;
                    info->return_type = from_typeref(function_decl->return_type);
                    info->owner_unit = unit.get();
                    info->function = function_decl;
                    info->external_only = !function_decl->template_params.empty();
                    for (const auto& parameter : function_decl->parameters) {
                        if (parameter.type.variadic) {
                            info->variadic = true;
                            info->external_only = true;
                            continue;
                        }
                        SemanticType param_type = from_typeref(parameter.type);
                        info->params.push_back(param_type);
                    }
                    info->llvm_name = mangle_free_function(*info);
                    free_functions_[info->name].push_back(info.get());
                    owned_functions_.push_back(std::move(info));
                    continue;
                }

                const auto* struct_decl = dynamic_cast<const StructDecl*>(decl.get());
                if (struct_decl == nullptr) {
                    continue;
                }
                StructInfo& owner = structs_[struct_decl->name];
                for (const auto& constructor_decl : struct_decl->constructors) {
                    auto info = std::make_unique<FunctionInfo>();
                    info->kind = FunctionKind::Constructor;
                    info->owner = struct_decl->name;
                    info->name = struct_decl->name;
                    info->return_type = SemanticType {struct_decl->name};
                    info->owner_unit = unit.get();
                    info->constructor = &constructor_decl;
                    for (const auto& parameter : constructor_decl.parameters) {
                        if (parameter.type.variadic) {
                            info->variadic = true;
                            info->external_only = true;
                            continue;
                        }
                        info->params.push_back(from_typeref(parameter.type));
                    }
                    info->llvm_name = mangle_constructor(*info);
                    owner.constructors.push_back(info.get());
                    owned_functions_.push_back(std::move(info));
                }
                for (const auto& method_decl : struct_decl->methods) {
                    auto info = std::make_unique<FunctionInfo>();
                    info->kind = FunctionKind::Method;
                    info->owner = struct_decl->name;
                    info->name = method_decl.name;
                    info->return_type = from_typeref(method_decl.return_type);
                    info->owner_unit = unit.get();
                    info->method = &method_decl;
                    for (const auto& parameter : method_decl.parameters) {
                        if (parameter.type.variadic) {
                            info->variadic = true;
                            info->external_only = true;
                            continue;
                        }
                        info->params.push_back(from_typeref(parameter.type));
                    }
                    info->llvm_name = mangle_method(*info);
                    owner.methods[method_decl.name].push_back(info.get());
                    owned_functions_.push_back(std::move(info));
                }
                for (const auto& destructor_decl : struct_decl->destructors) {
                    auto info = std::make_unique<FunctionInfo>();
                    info->kind = FunctionKind::Destructor;
                    info->owner = struct_decl->name;
                    info->name = destructor_decl.name;
                    info->return_type = SemanticType {"void"};
                    info->owner_unit = unit.get();
                    info->destructor = &destructor_decl;
                    info->llvm_name = mangle_destructor(*info);
                    owner.destructor = info.get();
                    owned_functions_.push_back(std::move(info));
                }
                for (const auto& conversion_decl : struct_decl->conversions) {
                    auto info = std::make_unique<FunctionInfo>();
                    info->kind = FunctionKind::Conversion;
                    info->owner = struct_decl->name;
                    info->name = "type_cast";
                    info->return_type = from_typeref(conversion_decl.target_type);
                    info->owner_unit = unit.get();
                    info->conversion = &conversion_decl;
                    info->llvm_name = mangle_conversion(*info);
                    owner.conversions[info->return_type.name] = info.get();
                    owned_functions_.push_back(std::move(info));
                }
            }
        }
    }

    std::string join_params(const std::vector<SemanticType>& params) const {
        std::string out;
        for (size_t i = 0; i < params.size(); ++i) {
            if (i != 0) {
                out += ",";
            }
            out += type_to_string(params[i]);
        }
        return out;
    }

    std::string mangle_free_function(const FunctionInfo& function) const {
        return std::format("{} {}({})", type_to_string(function.return_type), function.name, join_params(function.params));
    }

    std::string mangle_method(const FunctionInfo& function) const {
        return std::format("method {} {}.{}({})",
                           type_to_string(function.return_type),
                           function.owner,
                           function.name,
                           join_params(function.params));
    }

    std::string mangle_constructor(const FunctionInfo& function) const {
        return std::format("ctor {} {}({})", function.owner, function.name, join_params(function.params));
    }

    std::string mangle_destructor(const FunctionInfo& function) const {
        return std::format("dtor {} ~{}()", function.owner, function.owner);
    }

    std::string mangle_conversion(const FunctionInfo& function) const {
        return std::format("conv {} {}.type_cast()", type_to_string(function.return_type), function.owner);
    }

    llvm::Type* llvm_type(const SemanticType& type, bool decay_array = false) {
        if (type.is_reference || type.is_pointer) {
            return llvm::PointerType::get(context_, 0);
        }
        if (type.is_array) {
            if (decay_array) {
                return llvm::PointerType::get(context_, 0);
            }
            return llvm::ArrayType::get(llvm_type(element_type(type)), type.array_size);
        }
        if (type.name == "void") {
            return llvm::Type::getVoidTy(context_);
        }
        if (type.name == "bool") {
            return llvm::Type::getInt1Ty(context_);
        }
        if (type.name == "char" || type.name == "int8" || type.name == "uint8") {
            return llvm::Type::getInt8Ty(context_);
        }
        if (type.name == "int16" || type.name == "uint16") {
            return llvm::Type::getInt16Ty(context_);
        }
        if (type.name == "int32" || type.name == "uint32") {
            return llvm::Type::getInt32Ty(context_);
        }
        if (type.name == "int64" || type.name == "uint64") {
            return llvm::Type::getInt64Ty(context_);
        }
        if (type.name == "float") {
            return llvm::Type::getFloatTy(context_);
        }
        if (type.name == "double") {
            return llvm::Type::getDoubleTy(context_);
        }
        if (const auto found = structs_.find(type.name); found != structs_.end()) {
            return found->second.llvm_type;
        }
        errors_.push_back(std::format("Unknown Dino type '{}'", type_to_string(type)));
        return llvm::Type::getVoidTy(context_);
    }

    SemanticType element_type(const SemanticType& type) const {
        SemanticType result = type;
        result.is_array = false;
        result.array_size = 0;
        result.is_pointer = false;
        result.is_reference = false;
        return result;
    }

    llvm::Value* zero_value(const SemanticType& type) {
        if (type.is_void()) {
            return nullptr;
        }
        if (type.is_pointer || type.is_reference || type.is_array) {
            return llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0));
        }
        if (type.name == "float") {
            return llvm::ConstantFP::get(llvm::Type::getFloatTy(context_), 0.0);
        }
        if (type.name == "double") {
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context_), 0.0);
        }
        if (const auto* struct_type = llvm::dyn_cast<llvm::StructType>(llvm_type(type))) {
            return llvm::Constant::getNullValue(const_cast<llvm::StructType*>(struct_type));
        }
        return llvm::ConstantInt::get(llvm_type(type), 0);
    }

    llvm::AllocaInst* create_entry_alloca(llvm::Function* function, llvm::Type* type, const std::string& name) {
        llvm::IRBuilder<> entry_builder(&function->getEntryBlock(), function->getEntryBlock().begin());
        return entry_builder.CreateAlloca(type, nullptr, name);
    }

    void declare_structs() {
        for (auto& [name, info] : structs_) {
            info.llvm_type = llvm::StructType::create(context_, name);
        }
    }

    void define_struct_layouts() {
        for (auto& [name, info] : structs_) {
            std::vector<llvm::Type*> field_types;
            field_types.reserve(info.fields.size());
            for (const auto& field : info.fields) {
                field_types.push_back(llvm_type(field.type));
            }
            info.llvm_type->setBody(field_types, false);
        }
    }

    void declare_functions() {
        for (auto& function : owned_functions_) {
            std::vector<llvm::Type*> params;
            if (function->kind == FunctionKind::Method || function->kind == FunctionKind::Conversion ||
                function->kind == FunctionKind::Destructor) {
                params.push_back(llvm::PointerType::get(context_, 0));
            }
            for (const auto& parameter : function->params) {
                params.push_back(llvm_type(parameter, true));
            }
            llvm::FunctionType* function_type = llvm::FunctionType::get(llvm_type(function->return_type),
                                                                        params,
                                                                        function->variadic);
            function->llvm_function = llvm::Function::Create(function_type,
                                                             llvm::Function::ExternalLinkage,
                                                             function->llvm_name,
                                                             module_);
        }
    }

    void define_functions() {
        for (auto& function : owned_functions_) {
            if (function->external_only || function->llvm_function == nullptr) {
                continue;
            }
            if (function->function != nullptr) {
                define_free_function(*function);
            } else if (function->constructor != nullptr) {
                define_constructor(*function);
            } else if (function->destructor != nullptr) {
                define_destructor(*function);
            } else if (function->method != nullptr) {
                define_method(*function);
            } else if (function->conversion != nullptr) {
                define_conversion(*function);
            }
        }
    }

    void begin_function(FunctionInfo& info) {
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", info.llvm_function);
        builder_.SetInsertPoint(entry);
        current_function_ = info.llvm_function;
        scopes_.clear();
        push_scope();
        current_yield_ = nullptr;
    }

    void finish_function(FunctionInfo& info) {
        if (builder_.GetInsertBlock() != nullptr && builder_.GetInsertBlock()->getTerminator() == nullptr) {
            if (info.kind == FunctionKind::Constructor) {
                // Constructors always return the fully initialized value.
            } else if (info.return_type.is_void()) {
                builder_.CreateRetVoid();
            } else {
                builder_.CreateRet(zero_value(info.return_type));
            }
        }
        scopes_.clear();
        current_function_ = nullptr;
        current_self_ = nullptr;
        current_struct_ = nullptr;
        current_unit_ = nullptr;
        current_yield_ = nullptr;
    }

    void bind_parameters(const std::vector<SemanticType>& parameters,
                         const std::vector<std::string>& names,
                         size_t start_index = 0) {
        for (size_t i = 0; i < names.size(); ++i) {
            llvm::Argument* arg = current_function_->getArg(static_cast<unsigned>(i + start_index));
            arg->setName(names[i]);
            llvm::AllocaInst* slot = create_entry_alloca(current_function_, llvm_type(parameters[i], true), names[i]);
            builder_.CreateStore(arg, slot);
            declare_variable(names[i], VariableInfo {parameters[i], slot, false});
        }
    }

    void define_free_function(FunctionInfo& info) {
        begin_function(info);
        current_unit_ = info.owner_unit;
        std::vector<std::string> names;
        names.reserve(info.function->parameters.size());
        for (const auto& parameter : info.function->parameters) {
            names.push_back(parameter.name);
        }
        bind_parameters(info.params, names);
        emit_statement(info.function->body.get());
        finish_function(info);
    }

    void define_constructor(FunctionInfo& info) {
        const StructInfo& owner = structs_.at(info.owner);
        begin_function(info);
        current_unit_ = info.owner_unit;
        llvm::AllocaInst* self_slot = create_entry_alloca(current_function_, owner.llvm_type, "this.storage");
        current_self_ = self_slot;
        current_struct_ = &owner;
        declare_variable("this", VariableInfo {SemanticType {info.owner, false, false, true}, self_slot, false});

        std::vector<std::string> names;
        names.reserve(info.constructor->parameters.size());
        for (const auto& parameter : info.constructor->parameters) {
            names.push_back(parameter.name);
        }
        bind_parameters(info.params, names);
        emit_statement(info.constructor->body.get());
        if (builder_.GetInsertBlock() != nullptr && builder_.GetInsertBlock()->getTerminator() == nullptr) {
            builder_.CreateRet(builder_.CreateLoad(owner.llvm_type, self_slot, "ctor.ret"));
        }
        finish_function(info);
    }

    void define_destructor(FunctionInfo& info) {
        const StructInfo& owner = structs_.at(info.owner);
        begin_function(info);
        current_unit_ = info.owner_unit;
        llvm::Argument* this_arg = current_function_->getArg(0);
        this_arg->setName("this");
        current_self_ = this_arg;
        current_struct_ = &owner;
        declare_variable("this", VariableInfo {SemanticType {info.owner, false, false, true}, this_arg, false});
        emit_statement(info.destructor->body.get());
        finish_function(info);
    }

    void define_method(FunctionInfo& info) {
        const StructInfo& owner = structs_.at(info.owner);
        begin_function(info);
        current_unit_ = info.owner_unit;
        llvm::Argument* this_arg = current_function_->getArg(0);
        this_arg->setName("this");
        current_self_ = this_arg;
        current_struct_ = &owner;
        declare_variable("this", VariableInfo {SemanticType {info.owner, false, false, true}, this_arg, false});

        std::vector<std::string> names;
        names.reserve(info.method->parameters.size());
        for (const auto& parameter : info.method->parameters) {
            names.push_back(parameter.name);
        }
        bind_parameters(info.params, names, 1);
        emit_statement(info.method->body.get());
        finish_function(info);
    }

    void define_conversion(FunctionInfo& info) {
        const StructInfo& owner = structs_.at(info.owner);
        begin_function(info);
        current_unit_ = info.owner_unit;
        llvm::Argument* this_arg = current_function_->getArg(0);
        this_arg->setName("this");
        current_self_ = this_arg;
        current_struct_ = &owner;
        declare_variable("this", VariableInfo {SemanticType {info.owner, false, false, true}, this_arg, false});
        emit_statement(info.conversion->body.get());
        finish_function(info);
    }

    std::unordered_map<std::string, std::string> current_modules() const {
        std::unordered_map<std::string, std::string> modules;
        if (current_unit_ == nullptr) {
            return modules;
        }
        for (const auto& decl : current_unit_->declarations) {
            if (const auto* include = dynamic_cast<const IncludeDecl*>(decl.get())) {
                modules[module_alias_from_include(include->include_path)] = include->resolved_path;
            }
        }
        return modules;
    }

    bool is_module_identifier(const Expr* expr, std::string* resolved_path = nullptr) const {
        const auto* identifier = dynamic_cast<const IdentifierExpr*>(expr);
        if (identifier == nullptr) {
            return false;
        }
        const auto modules = current_modules();
        const auto found = modules.find(identifier->name);
        if (found == modules.end()) {
            return false;
        }
        if (resolved_path != nullptr) {
            *resolved_path = found->second;
        }
        return true;
    }

    const FieldInfo* lookup_field(const StructInfo& owner, const std::string& name) const {
        if (const auto found = owner.field_indices.find(name); found != owner.field_indices.end()) {
            return &owner.fields[found->second];
        }
        return nullptr;
    }

    SemanticType infer_expr_type(const Expr* expr) {
        if (expr == nullptr) {
            return SemanticType {"void"};
        }
        if (const auto* literal = dynamic_cast<const LiteralExpr*>(expr)) {
            if (literal->literal_kind == "String") {
                SemanticType type;
                type.name = "char";
                type.is_pointer = true;
                type.is_const = true;
                return type;
            }
            if (literal->literal_kind == "Character") {
                return SemanticType {"char"};
            }
            if (literal->literal_kind == "KwTrue" || literal->literal_kind == "KwFalse") {
                return SemanticType {"bool"};
            }
            if (literal->value.find('.') != std::string::npos) {
                return SemanticType {"double"};
            }
            return SemanticType {"int32"};
        }
        if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(expr)) {
            if (identifier->name == "this" && current_struct_ != nullptr && current_self_ != nullptr) {
                SemanticType self_type {current_struct_->decl->name};
                self_type.is_pointer = true;
                return self_type;
            }
            if (const VariableInfo* variable = lookup_variable(identifier->name)) {
                return variable->type;
            }
            if (current_struct_ != nullptr) {
                if (const FieldInfo* field = lookup_field(*current_struct_, identifier->name)) {
                    return field->type;
                }
            }
            if (structs_.contains(identifier->name)) {
                return SemanticType {"<type>"};
            }
            if (free_functions_.contains(identifier->name)) {
                return SemanticType {"<function>"};
            }
            if (is_module_identifier(expr)) {
                return SemanticType {"<module>"};
            }
            return SemanticType {identifier->name, false, false, false, false, false, 0, true};
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
            SemanticType operand = infer_expr_type(unary->operand.get());
            if (unary->op == "*" && (operand.is_pointer || operand.is_array || operand.is_reference)) {
                return element_type(operand);
            }
            if (unary->op == "&") {
                operand.is_pointer = true;
                operand.is_array = false;
                return operand;
            }
            if (unary->op == "!") {
                return SemanticType {"bool"};
            }
            return operand;
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
            SemanticType lhs = infer_expr_type(binary->lhs.get());
            SemanticType rhs = infer_expr_type(binary->rhs.get());
            if (binary->op == "=" || binary->op == "+=" || binary->op == "-=" || binary->op == "*=" || binary->op == "/=") {
                return lhs;
            }
            if (binary->op == "==" || binary->op == "!=" || binary->op == "<" || binary->op == ">" ||
                binary->op == "<=" || binary->op == ">=" || binary->op == "&&" || binary->op == "||") {
                return SemanticType {"bool"};
            }
            if (is_numeric_type(lhs) && is_numeric_type(rhs)) {
                return numeric_common_type(lhs, rhs);
            }
            return lhs;
        }
        if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
            SemanticType object_type = infer_expr_type(member->object.get());
            if (member->via_arrow) {
                object_type.is_pointer = false;
                object_type.is_reference = false;
            }
            object_type.is_array = false;
            object_type.array_size = 0;
            if (const auto found = structs_.find(object_type.name); found != structs_.end()) {
                if (const FieldInfo* field = lookup_field(found->second, member->member)) {
                    return field->type;
                }
                if (const auto methods = found->second.methods.find(member->member); methods != found->second.methods.end() &&
                    !methods->second.empty()) {
                    return methods->second.front()->return_type;
                }
            }
            return SemanticType {"<error>", false, false, false, false, false, 0, true};
        }
        if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
            return element_type(infer_expr_type(index->object.get()));
        }
        if (const auto* cast = dynamic_cast<const TypeCastExpr*>(expr)) {
            return from_typeref(cast->target_type);
        }
        if (const auto* alloc = dynamic_cast<const NewExpr*>(expr)) {
            SemanticType type = from_typeref(alloc->target_type);
            type.is_pointer = true;
            return type;
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
            CallResolution resolution = resolve_call(call);
            if (resolution.function != nullptr) {
                return resolution.function->return_type;
            }
        }
        if (const auto* if_expr = dynamic_cast<const IfExpr*>(expr)) {
            SemanticType then_type = infer_branch_type(if_expr->then_branch);
            if (!if_expr->else_branch.has_value()) {
                return then_type;
            }
            SemanticType else_type = infer_branch_type(*if_expr->else_branch);
            if (same_type(then_type, else_type)) {
                return then_type;
            }
            if (is_numeric_type(then_type) && is_numeric_type(else_type)) {
                return numeric_common_type(then_type, else_type);
            }
            return then_type;
        }
        if (const auto* match_expr = dynamic_cast<const MatchExpr*>(expr)) {
            for (const auto& match_case : match_expr->cases) {
                if (match_case.fallthrough) {
                    continue;
                }
                return infer_branch_type(match_case.body);
            }
        }
        return SemanticType {"<error>", false, false, false, false, false, 0, true};
    }

    SemanticType infer_branch_type(const std::variant<ExprPtr, std::unique_ptr<BlockStmt>>& branch) {
        if (const auto* expr = std::get_if<ExprPtr>(&branch)) {
            return infer_expr_type(expr->get());
        }
        return infer_block_yield_type(std::get<std::unique_ptr<BlockStmt>>(branch).get());
    }

    SemanticType infer_block_yield_type(const BlockStmt* block) {
        if (block == nullptr) {
            return SemanticType {"void"};
        }
        for (const auto& statement : block->statements) {
            if (const auto* yield = dynamic_cast<const YieldStmt*>(statement.get())) {
                return infer_expr_type(yield->value.get());
            }
            if (const auto* nested = dynamic_cast<const BlockStmt*>(statement.get())) {
                SemanticType nested_type = infer_block_yield_type(nested);
                if (!nested_type.is_void()) {
                    return nested_type;
                }
            }
        }
        return SemanticType {"void"};
    }

    llvm::Value* emit_literal(const LiteralExpr& literal) {
        if (literal.literal_kind == "String") {
            std::string unescaped = literal.value;
            if (unescaped.size() >= 2 && unescaped.front() == '"' && unescaped.back() == '"') {
                unescaped = unescaped.substr(1, unescaped.size() - 2);
            }
            llvm::GlobalVariable* global = builder_.CreateGlobalString(unescaped, std::format(".str.{}", string_id_++));
            return builder_.CreateConstInBoundsGEP2_32(global->getValueType(), global, 0, 0, "str.ptr");
        }
        if (literal.literal_kind == "Character") {
            return llvm::ConstantInt::get(llvm::Type::getInt8Ty(context_), static_cast<unsigned char>(literal.value[1]));
        }
        if (literal.literal_kind == "KwTrue") {
            return llvm::ConstantInt::getTrue(context_);
        }
        if (literal.literal_kind == "KwFalse") {
            return llvm::ConstantInt::getFalse(context_);
        }
        if (literal.value.find('.') != std::string::npos) {
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context_), std::stod(literal.value));
        }
        return llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), std::stoll(literal.value), true);
    }

    llvm::Value* load_variable(const VariableInfo& variable, const std::string& name) {
        if (variable.type.is_array) {
            llvm::Type* array_type = llvm_type(variable.type);
            return builder_.CreateInBoundsGEP(array_type, variable.address, {builder_.getInt32(0), builder_.getInt32(0)}, name + ".decay");
        }
        return builder_.CreateLoad(llvm_type(variable.type), variable.address, name);
    }

    llvm::Value* emit_lvalue(const Expr* expr) {
        if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(expr)) {
            if (identifier->name == "this" && current_self_ != nullptr) {
                return current_self_;
            }
            if (VariableInfo* variable = lookup_variable(identifier->name)) {
                return variable->address;
            }
            if (current_struct_ != nullptr && current_self_ != nullptr) {
                if (const FieldInfo* field = lookup_field(*current_struct_, identifier->name)) {
                    return builder_.CreateStructGEP(current_struct_->llvm_type, current_self_, field->index, identifier->name + ".addr");
                }
            }
        }
        if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
            SemanticType object_type = infer_expr_type(member->object.get());
            llvm::Value* base_address = nullptr;
            if (member->via_arrow) {
                base_address = emit_expression(member->object.get());
                object_type.is_pointer = false;
                object_type.is_reference = false;
            } else {
                base_address = emit_lvalue(member->object.get());
            }
            if (base_address == nullptr) {
                errors_.push_back(std::format("Cannot take address of member '{}'", member->member));
                return nullptr;
            }
            const auto found = structs_.find(object_type.name);
            if (found == structs_.end()) {
                errors_.push_back(std::format("Unknown struct type '{}'", object_type.name));
                return nullptr;
            }
            const FieldInfo* field = lookup_field(found->second, member->member);
            if (field == nullptr) {
                errors_.push_back(std::format("Struct '{}' has no field '{}'", object_type.name, member->member));
                return nullptr;
            }
            return builder_.CreateStructGEP(found->second.llvm_type, base_address, field->index, member->member + ".addr");
        }
        if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
            SemanticType object_type = infer_expr_type(index->object.get());
            llvm::Value* index_value = emit_expression(index->index.get());
            if (index_value == nullptr) {
                return nullptr;
            }
            if (llvm::Type* index_type = index_value->getType(); !index_type->isIntegerTy(32)) {
                index_value = builder_.CreateIntCast(index_value, llvm::Type::getInt32Ty(context_), true, "idx.cast");
            }
            if (object_type.is_array) {
                llvm::Value* array_address = emit_lvalue(index->object.get());
                if (array_address == nullptr) {
                    return nullptr;
                }
                return builder_.CreateInBoundsGEP(llvm_type(object_type),
                                                  array_address,
                                                  {builder_.getInt32(0), index_value},
                                                  "array.elem.addr");
            }
            llvm::Value* pointer_value = emit_expression(index->object.get());
            return builder_.CreateInBoundsGEP(llvm_type(element_type(object_type)),
                                              pointer_value,
                                              index_value,
                                              "ptr.elem.addr");
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr); unary != nullptr && unary->op == "*") {
            return emit_expression(unary->operand.get());
        }
        errors_.push_back("Expression is not assignable");
        return nullptr;
    }

    llvm::Value* emit_expression(const Expr* expr) {
        if (expr == nullptr) {
            return nullptr;
        }
        if (const auto* literal = dynamic_cast<const LiteralExpr*>(expr)) {
            return emit_literal(*literal);
        }
        if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(expr)) {
            if (identifier->name == "this" && current_self_ != nullptr) {
                return current_self_;
            }
            if (const VariableInfo* variable = lookup_variable(identifier->name)) {
                return load_variable(*variable, identifier->name);
            }
            if (current_struct_ != nullptr && current_self_ != nullptr) {
                if (const FieldInfo* field = lookup_field(*current_struct_, identifier->name)) {
                    llvm::Value* address = builder_.CreateStructGEP(current_struct_->llvm_type, current_self_, field->index, identifier->name + ".addr");
                    return builder_.CreateLoad(llvm_type(field->type), address, identifier->name);
                }
            }
            errors_.push_back(std::format("Unknown identifier '{}'", identifier->name));
            return nullptr;
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
            return emit_unary(*unary);
        }
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
            return emit_binary(*binary);
        }
        if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
            return emit_call(*call);
        }
        if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
            llvm::Value* address = emit_lvalue(member);
            if (address == nullptr) {
                return nullptr;
            }
            SemanticType type = infer_expr_type(member);
            return builder_.CreateLoad(llvm_type(type), address, member->member);
        }
        if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
            llvm::Value* address = emit_lvalue(index);
            if (address == nullptr) {
                return nullptr;
            }
            SemanticType type = infer_expr_type(index);
            return builder_.CreateLoad(llvm_type(type), address, "index.load");
        }
        if (const auto* cast = dynamic_cast<const TypeCastExpr*>(expr)) {
            return emit_type_cast(*cast);
        }
        if (const auto* alloc = dynamic_cast<const NewExpr*>(expr)) {
            return emit_new_expression(*alloc);
        }
        if (const auto* if_expr = dynamic_cast<const IfExpr*>(expr)) {
            return emit_if_expression(*if_expr);
        }
        if (const auto* match_expr = dynamic_cast<const MatchExpr*>(expr)) {
            return emit_match_expression(*match_expr);
        }
        errors_.push_back(std::format("Unsupported expression kind '{}'", expr->kind()));
        return nullptr;
    }

    llvm::Value* emit_unary(const UnaryExpr& unary) {
        if (unary.op == "&") {
            return emit_lvalue(unary.operand.get());
        }
        if (unary.op == "*") {
            llvm::Value* pointer = emit_expression(unary.operand.get());
            SemanticType result_type = infer_expr_type(&unary);
            return builder_.CreateLoad(llvm_type(result_type), pointer, "deref");
        }
        if (unary.op == "++" || unary.op == "--") {
            llvm::Value* address = emit_lvalue(unary.operand.get());
            SemanticType type = infer_expr_type(unary.operand.get());
            llvm::Value* old_value = builder_.CreateLoad(llvm_type(type), address, "inc.old");
            llvm::Value* step = is_float_type(type) ? llvm::ConstantFP::get(llvm_type(type), 1.0)
                                                    : llvm::ConstantInt::get(llvm_type(type), 1);
            llvm::Value* new_value = nullptr;
            if (is_float_type(type)) {
                new_value = unary.op == "++" ? builder_.CreateFAdd(old_value, step, "inc.new")
                                              : builder_.CreateFSub(old_value, step, "dec.new");
            } else {
                new_value = unary.op == "++" ? builder_.CreateAdd(old_value, step, "inc.new")
                                              : builder_.CreateSub(old_value, step, "dec.new");
            }
            builder_.CreateStore(new_value, address);
            return unary.postfix ? old_value : new_value;
        }
        llvm::Value* operand = emit_expression(unary.operand.get());
        SemanticType operand_type = infer_expr_type(unary.operand.get());
        if (operand == nullptr) {
            return nullptr;
        }
        if (unary.op == "+") {
            return operand;
        }
        if (unary.op == "-") {
            return is_float_type(operand_type) ? builder_.CreateFNeg(operand, "neg") : builder_.CreateNeg(operand, "neg");
        }
        if (unary.op == "!") {
            return builder_.CreateNot(as_boolean(operand, operand_type), "not");
        }
        if (unary.op == "~") {
            return builder_.CreateNot(operand, "bitnot");
        }
        if (unary.op == "...") {
            return operand;
        }
        errors_.push_back(std::format("Unsupported unary operator '{}'", unary.op));
        return nullptr;
    }

    llvm::Value* emit_binary(const BinaryExpr& binary) {
        if (binary.op == "=" || binary.op == "+=" || binary.op == "-=" || binary.op == "*=" || binary.op == "/=" ||
            binary.op == "%=" || binary.op == "&=" || binary.op == "|=" || binary.op == "^=") {
            return emit_assignment(binary);
        }
        if (binary.op == "&&" || binary.op == "||") {
            return emit_logical(binary);
        }
        llvm::Value* lhs = emit_expression(binary.lhs.get());
        llvm::Value* rhs = emit_expression(binary.rhs.get());
        SemanticType lhs_type = infer_expr_type(binary.lhs.get());
        SemanticType rhs_type = infer_expr_type(binary.rhs.get());
        if (lhs == nullptr || rhs == nullptr) {
            return nullptr;
        }
        if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
            SemanticType common = numeric_common_type(lhs_type, rhs_type);
            lhs = cast_value(lhs, lhs_type, common, true);
            rhs = cast_value(rhs, rhs_type, common, true);
            if (binary.op == "+") {
                return is_float_type(common) ? builder_.CreateFAdd(lhs, rhs, "add") : builder_.CreateAdd(lhs, rhs, "add");
            }
            if (binary.op == "-") {
                return is_float_type(common) ? builder_.CreateFSub(lhs, rhs, "sub") : builder_.CreateSub(lhs, rhs, "sub");
            }
            if (binary.op == "*") {
                return is_float_type(common) ? builder_.CreateFMul(lhs, rhs, "mul") : builder_.CreateMul(lhs, rhs, "mul");
            }
            if (binary.op == "/") {
                return is_float_type(common) ? builder_.CreateFDiv(lhs, rhs, "div")
                                             : builder_.CreateSDiv(lhs, rhs, "div");
            }
            if (binary.op == "%") {
                return is_float_type(common) ? builder_.CreateFRem(lhs, rhs, "rem")
                                             : builder_.CreateSRem(lhs, rhs, "rem");
            }
            if (binary.op == "<") {
                return is_float_type(common) ? builder_.CreateFCmpULT(lhs, rhs, "lt")
                                             : builder_.CreateICmpSLT(lhs, rhs, "lt");
            }
            if (binary.op == "<=") {
                return is_float_type(common) ? builder_.CreateFCmpULE(lhs, rhs, "le")
                                             : builder_.CreateICmpSLE(lhs, rhs, "le");
            }
            if (binary.op == ">") {
                return is_float_type(common) ? builder_.CreateFCmpUGT(lhs, rhs, "gt")
                                             : builder_.CreateICmpSGT(lhs, rhs, "gt");
            }
            if (binary.op == ">=") {
                return is_float_type(common) ? builder_.CreateFCmpUGE(lhs, rhs, "ge")
                                             : builder_.CreateICmpSGE(lhs, rhs, "ge");
            }
            if (binary.op == "==") {
                return is_float_type(common) ? builder_.CreateFCmpUEQ(lhs, rhs, "eq")
                                             : builder_.CreateICmpEQ(lhs, rhs, "eq");
            }
            if (binary.op == "!=") {
                return is_float_type(common) ? builder_.CreateFCmpUNE(lhs, rhs, "ne")
                                             : builder_.CreateICmpNE(lhs, rhs, "ne");
            }
            if (binary.op == "&") {
                return builder_.CreateAnd(lhs, rhs, "and");
            }
            if (binary.op == "|") {
                return builder_.CreateOr(lhs, rhs, "or");
            }
            if (binary.op == "^") {
                return builder_.CreateXor(lhs, rhs, "xor");
            }
            if (binary.op == "<<") {
                return builder_.CreateShl(lhs, rhs, "shl");
            }
            if (binary.op == ">>") {
                return builder_.CreateAShr(lhs, rhs, "shr");
            }
        }
        errors_.push_back(std::format("Unsupported binary operator '{}' for {} and {}",
                                      binary.op,
                                      type_to_string(lhs_type),
                                      type_to_string(rhs_type)));
        return nullptr;
    }

    llvm::Value* emit_assignment(const BinaryExpr& binary) {
        llvm::Value* address = emit_lvalue(binary.lhs.get());
        SemanticType lhs_type = infer_expr_type(binary.lhs.get());
        if (address == nullptr) {
            return nullptr;
        }
        llvm::Value* rhs = emit_expression(binary.rhs.get());
        SemanticType rhs_type = infer_expr_type(binary.rhs.get());
        if (rhs == nullptr) {
            return nullptr;
        }

        llvm::Value* result = nullptr;
        if (binary.op == "=") {
            result = cast_value(rhs, rhs_type, lhs_type, true);
        } else {
            llvm::Value* lhs = builder_.CreateLoad(llvm_type(lhs_type), address, "assign.old");
            llvm::Value* converted_rhs = cast_value(rhs, rhs_type, lhs_type, true);
            if (binary.op == "+=") {
                result = is_float_type(lhs_type) ? builder_.CreateFAdd(lhs, converted_rhs) : builder_.CreateAdd(lhs, converted_rhs);
            } else if (binary.op == "-=") {
                result = is_float_type(lhs_type) ? builder_.CreateFSub(lhs, converted_rhs) : builder_.CreateSub(lhs, converted_rhs);
            } else if (binary.op == "*=") {
                result = is_float_type(lhs_type) ? builder_.CreateFMul(lhs, converted_rhs) : builder_.CreateMul(lhs, converted_rhs);
            } else if (binary.op == "/=") {
                result = is_float_type(lhs_type) ? builder_.CreateFDiv(lhs, converted_rhs) : builder_.CreateSDiv(lhs, converted_rhs);
            } else if (binary.op == "%=") {
                result = is_float_type(lhs_type) ? builder_.CreateFRem(lhs, converted_rhs) : builder_.CreateSRem(lhs, converted_rhs);
            } else if (binary.op == "&=") {
                result = builder_.CreateAnd(lhs, converted_rhs);
            } else if (binary.op == "|=") {
                result = builder_.CreateOr(lhs, converted_rhs);
            } else if (binary.op == "^=") {
                result = builder_.CreateXor(lhs, converted_rhs);
            } else {
                errors_.push_back(std::format("Compound assignment '{}' is not implemented yet", binary.op));
                return nullptr;
            }
        }
        builder_.CreateStore(result, address);
        return result;
    }

    llvm::Value* emit_logical(const BinaryExpr& binary) {
        llvm::Value* lhs_value = emit_expression(binary.lhs.get());
        if (lhs_value == nullptr) {
            return nullptr;
        }
        SemanticType lhs_type = infer_expr_type(binary.lhs.get());
        llvm::Function* function = current_function_;
        llvm::BasicBlock* lhs_block = builder_.GetInsertBlock();
        llvm::BasicBlock* rhs_block = llvm::BasicBlock::Create(context_, binary.op == "&&" ? "and.rhs" : "or.rhs", function);
        llvm::BasicBlock* merge_block = llvm::BasicBlock::Create(context_, binary.op == "&&" ? "and.merge" : "or.merge", function);
        llvm::Value* lhs_bool = as_boolean(lhs_value, lhs_type);
        if (binary.op == "&&") {
            builder_.CreateCondBr(lhs_bool, rhs_block, merge_block);
        } else {
            builder_.CreateCondBr(lhs_bool, merge_block, rhs_block);
        }

        builder_.SetInsertPoint(rhs_block);
        llvm::Value* rhs_value = emit_expression(binary.rhs.get());
        SemanticType rhs_type = infer_expr_type(binary.rhs.get());
        llvm::Value* rhs_bool = as_boolean(rhs_value, rhs_type);
        builder_.CreateBr(merge_block);
        llvm::BasicBlock* rhs_end = builder_.GetInsertBlock();

        builder_.SetInsertPoint(merge_block);
        llvm::PHINode* phi = builder_.CreatePHI(llvm::Type::getInt1Ty(context_), 2, "logic.result");
        phi->addIncoming(binary.op == "&&" ? llvm::ConstantInt::getFalse(context_) : llvm::ConstantInt::getTrue(context_), lhs_block);
        phi->addIncoming(rhs_bool, rhs_end);
        return phi;
    }


    llvm::Value* emit_if_expression(const IfExpr& expr) {
        SemanticType result_type = infer_expr_type(&expr);
        llvm::AllocaInst* slot = create_entry_alloca(current_function_, llvm_type(result_type), "if.expr");
        builder_.CreateStore(zero_value(result_type), slot);

        llvm::Value* condition = emit_expression(expr.condition.get());
        if (condition == nullptr) {
            return nullptr;
        }
        condition = as_boolean(condition, infer_expr_type(expr.condition.get()));
        llvm::Function* function = current_function_;
        llvm::BasicBlock* then_block = llvm::BasicBlock::Create(context_, "if.then", function);
        llvm::BasicBlock* else_block = llvm::BasicBlock::Create(context_, "if.else", function);
        llvm::BasicBlock* merge_block = llvm::BasicBlock::Create(context_, "if.merge", function);
        builder_.CreateCondBr(condition, then_block, else_block);

        builder_.SetInsertPoint(then_block);
        emit_branch_value(expr.then_branch, result_type, slot, merge_block);
        if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            builder_.CreateBr(merge_block);
        }

        builder_.SetInsertPoint(else_block);
        if (expr.else_branch.has_value()) {
            emit_branch_value(*expr.else_branch, result_type, slot, merge_block);
        }
        if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            builder_.CreateBr(merge_block);
        }

        builder_.SetInsertPoint(merge_block);
        return builder_.CreateLoad(llvm_type(result_type), slot, "if.result");
    }

    llvm::Value* emit_match_expression(const MatchExpr& expr) {
        SemanticType result_type = infer_expr_type(&expr);
        llvm::AllocaInst* slot = create_entry_alloca(current_function_, llvm_type(result_type), "match.expr");
        builder_.CreateStore(zero_value(result_type), slot);

        llvm::Value* subject = emit_expression(expr.subject.get());
        SemanticType subject_type = infer_expr_type(expr.subject.get());
        llvm::Function* function = current_function_;
        llvm::BasicBlock* merge_block = llvm::BasicBlock::Create(context_, "match.merge", function);
        llvm::BasicBlock* dispatch_block = builder_.GetInsertBlock();

        std::vector<llvm::BasicBlock*> case_blocks;
        case_blocks.reserve(expr.cases.size());
        for (size_t i = 0; i < expr.cases.size(); ++i) {
            case_blocks.push_back(llvm::BasicBlock::Create(context_, std::format("match.case.{}", i), function));
        }
        llvm::BasicBlock* default_block = merge_block;

        for (size_t i = 0; i < expr.cases.size(); ++i) {
            const MatchCase& match_case = expr.cases[i];
            if (match_case.is_default) {
                default_block = case_blocks[i];
                continue;
            }
            builder_.SetInsertPoint(dispatch_block);
            llvm::Value* case_value = emit_expression(match_case.match_expr.get());
            SemanticType case_type = infer_expr_type(match_case.match_expr.get());
            SemanticType common = is_numeric_type(subject_type) && is_numeric_type(case_type)
                                      ? numeric_common_type(subject_type, case_type)
                                      : subject_type;
            llvm::Value* lhs = cast_value(subject, subject_type, common, true);
            llvm::Value* rhs = cast_value(case_value, case_type, common, true);
            llvm::Value* condition = is_float_type(common) ? builder_.CreateFCmpUEQ(lhs, rhs)
                                                           : builder_.CreateICmpEQ(lhs, rhs);
            llvm::BasicBlock* next_dispatch = llvm::BasicBlock::Create(context_, std::format("match.dispatch.{}", i), function);
            builder_.CreateCondBr(condition, case_blocks[i], next_dispatch);
            dispatch_block = next_dispatch;
        }
        builder_.SetInsertPoint(dispatch_block);
        builder_.CreateBr(default_block);

        for (size_t i = 0; i < expr.cases.size(); ++i) {
            builder_.SetInsertPoint(case_blocks[i]);
            const MatchCase& match_case = expr.cases[i];
            if (match_case.fallthrough) {
                llvm::BasicBlock* next = (i + 1 < expr.cases.size()) ? case_blocks[i + 1] : merge_block;
                builder_.CreateBr(next);
                continue;
            }
            emit_branch_value(match_case.body, result_type, slot, merge_block);
            if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
                builder_.CreateBr(merge_block);
            }
        }

        builder_.SetInsertPoint(merge_block);
        return builder_.CreateLoad(llvm_type(result_type), slot, "match.result");
    }

    void emit_branch_value(const std::variant<ExprPtr, std::unique_ptr<BlockStmt>>& branch,
                           const SemanticType& result_type,
                           llvm::AllocaInst* slot,
                           llvm::BasicBlock* merge_block) {
        if (const auto* expr = std::get_if<ExprPtr>(&branch)) {
            llvm::Value* value = emit_expression(expr->get());
            SemanticType actual_type = infer_expr_type(expr->get());
            if (value != nullptr) {
                builder_.CreateStore(cast_value(value, actual_type, result_type, true), slot);
            }
            return;
        }
        YieldContext yield_context {slot, merge_block, result_type, scopes_.size()};
        YieldContext* previous = current_yield_;
        current_yield_ = &yield_context;
        emit_statement(std::get<std::unique_ptr<BlockStmt>>(branch).get());
        current_yield_ = previous;
    }

    llvm::Value* emit_type_cast(const TypeCastExpr& cast) {
        llvm::Value* value = emit_expression(cast.value.get());
        SemanticType from = infer_expr_type(cast.value.get());
        SemanticType to = from_typeref(cast.target_type);
        return cast_value(value, from, to, false);
    }

    llvm::Value* emit_new_expression(const NewExpr& expr) {
        SemanticType allocated_type = from_typeref(expr.target_type);
        llvm::Type* storage_type = llvm_type(allocated_type);
        const llvm::DataLayout& layout = module_.getDataLayout();
        uint64_t size = layout.getTypeAllocSize(storage_type);
        llvm::Value* raw = builder_.CreateCall(ensure_malloc(),
                                               {llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), size)},
                                               "new.raw");
        llvm::Value* storage = builder_.CreateBitCast(raw, llvm::PointerType::get(context_, 0), "new.ptr");

        if (const auto struct_it = structs_.find(allocated_type.name); struct_it != structs_.end()) {
            std::vector<SemanticType> arg_types;
            arg_types.reserve(expr.args.size());
            for (const auto& arg : expr.args) {
                arg_types.push_back(infer_expr_type(arg.get()));
            }
            FunctionInfo* ctor = choose_overload(struct_it->second.constructors, arg_types);
            if (ctor == nullptr) {
                errors_.push_back(std::format("Unable to resolve constructor for new {}", allocated_type.name));
                return storage;
            }

            std::vector<llvm::Value*> args;
            args.reserve(expr.args.size());
            for (size_t i = 0; i < expr.args.size(); ++i) {
                llvm::Value* value = emit_expression(expr.args[i].get());
                value = cast_value(value, arg_types[i], ctor->params[i], true);
                args.push_back(value);
            }
            llvm::Value* constructed = builder_.CreateCall(ctor->llvm_function, args, "new.value");
            builder_.CreateStore(constructed, storage);
            return storage;
        }

        if (expr.args.size() == 1) {
            llvm::Value* init = emit_expression(expr.args[0].get());
            SemanticType init_type = infer_expr_type(expr.args[0].get());
            builder_.CreateStore(cast_value(init, init_type, allocated_type, true), storage);
        } else {
            builder_.CreateStore(zero_value(allocated_type), storage);
        }
        return storage;
    }

    llvm::Value* cast_value(llvm::Value* value, const SemanticType& from, const SemanticType& to, bool implicit) {
        if (value == nullptr || same_type(from, to)) {
            return value;
        }
        if (is_numeric_type(from) && is_numeric_type(to)) {
            if (is_float_type(from) && is_float_type(to)) {
                return to.name == "float" ? builder_.CreateFPTrunc(value, llvm_type(to), "fp.cast")
                                           : builder_.CreateFPExt(value, llvm_type(to), "fp.cast");
            }
            if (is_float_type(from) && is_integer_type(to)) {
                return is_signed_integer_type(to) ? builder_.CreateFPToSI(value, llvm_type(to), "fp2int")
                                                  : builder_.CreateFPToUI(value, llvm_type(to), "fp2int");
            }
            if (is_integer_type(from) && is_float_type(to)) {
                return is_signed_integer_type(from) ? builder_.CreateSIToFP(value, llvm_type(to), "int2fp")
                                                    : builder_.CreateUIToFP(value, llvm_type(to), "int2fp");
            }
            if (is_integer_type(from) && is_integer_type(to)) {
                const unsigned from_bits = llvm_type(from)->getIntegerBitWidth();
                const unsigned to_bits = llvm_type(to)->getIntegerBitWidth();
                if (from_bits == to_bits) {
                    return value;
                }
                if (from_bits < to_bits) {
                    return builder_.CreateIntCast(value, llvm_type(to), is_signed_integer_type(from), "int.extend");
                }
                return builder_.CreateTrunc(value, llvm_type(to), "int.trunc");
            }
        }
        if (!implicit && !from.is_pointer && !from.is_reference && !from.is_array) {
            if (const auto struct_it = structs_.find(from.name); struct_it != structs_.end()) {
                if (const auto conversion = struct_it->second.conversions.find(to.name); conversion != struct_it->second.conversions.end()) {
                    llvm::Value* address = materialize_address(value, from, "cast.tmp");
                    return builder_.CreateCall(conversion->second->llvm_function, {address}, "cast.call");
                }
            }
        }
        if (from.is_array && to.is_pointer && from.name == to.name) {
            return value;
        }
        errors_.push_back(std::format("Cannot {} cast from {} to {}",
                                      implicit ? "implicitly" : "explicitly",
                                      type_to_string(from),
                                      type_to_string(to)));
        return value;
    }

    llvm::Value* materialize_address(llvm::Value* value, const SemanticType& type, const std::string& name) {
        if (value == nullptr) {
            return nullptr;
        }
        if (type.is_pointer || type.is_reference) {
            return value;
        }
        llvm::AllocaInst* slot = create_entry_alloca(current_function_, llvm_type(type), name);
        builder_.CreateStore(value, slot);
        return slot;
    }

    llvm::Value* as_boolean(llvm::Value* value, const SemanticType& type) {
        if (type.name == "bool" && !type.is_pointer && !type.is_reference && !type.is_array) {
            return value;
        }
        if (is_float_type(type)) {
            return builder_.CreateFCmpUNE(value, llvm::ConstantFP::get(llvm_type(type), 0.0), "bool.cast");
        }
        if (is_integer_type(type)) {
            return builder_.CreateICmpNE(value, llvm::ConstantInt::get(llvm_type(type), 0), "bool.cast");
        }
        if (type.is_pointer || type.is_reference) {
            return builder_.CreateICmpNE(value,
                                         llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)),
                                         "bool.cast");
        }
        errors_.push_back(std::format("Cannot use type '{}' as condition", type_to_string(type)));
        return llvm::ConstantInt::getFalse(context_);
    }

    CallResolution resolve_call(const CallExpr* call) {
        CallResolution resolution;
        if (call == nullptr) {
            return resolution;
        }
        std::vector<SemanticType> arg_types;
        arg_types.reserve(call->args.size());
        for (const auto& argument : call->args) {
            arg_types.push_back(infer_expr_type(argument.get()));
        }

        if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(call->callee.get())) {
            if (const auto free_it = free_functions_.find(identifier->name); free_it != free_functions_.end()) {
                resolution.function = choose_overload(free_it->second, arg_types);
                return resolution;
            }
            if (const auto struct_it = structs_.find(identifier->name); struct_it != structs_.end()) {
                resolution.function = choose_overload(struct_it->second.constructors, arg_types);
                return resolution;
            }
            return resolution;
        }

        if (const auto* member = dynamic_cast<const MemberExpr*>(call->callee.get())) {
            std::string module_path;
            if (is_module_identifier(member->object.get(), &module_path)) {
                if (const auto free_it = free_functions_.find(member->member); free_it != free_functions_.end()) {
                    resolution.function = choose_overload(free_it->second, arg_types);
                }
                return resolution;
            }

            SemanticType object_type = infer_expr_type(member->object.get());
            if (member->via_arrow) {
                object_type.is_pointer = false;
                object_type.is_reference = false;
            }
            if (const auto struct_it = structs_.find(object_type.name); struct_it != structs_.end()) {
                if (const auto methods = struct_it->second.methods.find(member->member); methods != struct_it->second.methods.end()) {
                    resolution.function = choose_overload(methods->second, arg_types);
                    resolution.object_type = object_type;
                    resolution.object_address = member->via_arrow ? emit_expression(member->object.get())
                                                                  : ensure_address(member->object.get(), infer_expr_type(member->object.get()));
                }
            }
        }
        return resolution;
    }

    template <typename Container>
    FunctionInfo* choose_overload(const Container& overloads, const std::vector<SemanticType>& args) {
        for (FunctionInfo* function : overloads) {
            if (function == nullptr) {
                continue;
            }
            if (!function->variadic && function->params.size() != args.size()) {
                continue;
            }
            if (function->variadic && args.size() < function->params.size()) {
                continue;
            }
            bool matches = true;
            const size_t fixed_params = function->params.size();
            for (size_t i = 0; i < fixed_params; ++i) {
                if (!is_assignable_to(args[i], function->params[i]) && !same_type(args[i], function->params[i])) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return function;
            }
        }
        return nullptr;
    }

    llvm::Value* ensure_address(const Expr* expr, const SemanticType& type) {
        if (llvm::Value* lvalue = emit_lvalue(expr)) {
            return lvalue;
        }
        llvm::Value* value = emit_expression(expr);
        return materialize_address(value, type, "tmp.addr");
    }

    llvm::Value* emit_call(const CallExpr& call) {
        CallResolution resolution = resolve_call(&call);
        if (resolution.function == nullptr || resolution.function->llvm_function == nullptr) {
            errors_.push_back("Unable to resolve function call");
            return nullptr;
        }

        std::vector<llvm::Value*> args;
        if (resolution.function->kind == FunctionKind::Method || resolution.function->kind == FunctionKind::Conversion) {
            if (resolution.object_address == nullptr) {
                errors_.push_back("Method call requires addressable object");
                return nullptr;
            }
            args.push_back(resolution.object_address);
        }

        for (size_t i = 0; i < call.args.size(); ++i) {
            llvm::Value* value = emit_expression(call.args[i].get());
            SemanticType actual_type = infer_expr_type(call.args[i].get());
            if (value == nullptr) {
                return nullptr;
            }
            if (!resolution.function->variadic || i < resolution.function->params.size() - 1) {
                const SemanticType& expected_type = resolution.function->params[i];
                value = cast_value(value, actual_type, expected_type, true);
            }
            args.push_back(value);
        }

        if (resolution.function->return_type.is_void()) {
            builder_.CreateCall(resolution.function->llvm_function, args);
            return nullptr;
        }
        return builder_.CreateCall(resolution.function->llvm_function, args, "call");
    }

    void emit_statement(const Stmt* statement) {
        if (statement == nullptr || builder_.GetInsertBlock()->getTerminator() != nullptr) {
            return;
        }
        if (const auto* block = dynamic_cast<const BlockStmt*>(statement)) {
            push_scope();
            for (const auto& nested : block->statements) {
                emit_statement(nested.get());
                if (builder_.GetInsertBlock()->getTerminator() != nullptr) {
                    break;
                }
            }
            pop_scope();
            return;
        }
        if (const auto* expression = dynamic_cast<const ExprStmt*>(statement)) {
            emit_expression(expression->expr.get());
            return;
        }
        if (const auto* variable = dynamic_cast<const VarDeclStmt*>(statement)) {
            emit_var_decl(*variable);
            return;
        }
        if (const auto* return_stmt = dynamic_cast<const ReturnStmt*>(statement)) {
            emit_return(*return_stmt);
            return;
        }
        if (const auto* yield_stmt = dynamic_cast<const YieldStmt*>(statement)) {
            emit_yield(*yield_stmt);
            return;
        }
        if (const auto* delete_stmt = dynamic_cast<const DeleteStmt*>(statement)) {
            emit_delete(*delete_stmt);
            return;
        }
        if (const auto* if_stmt = dynamic_cast<const IfStmt*>(statement)) {
            emit_if_statement(*if_stmt);
            return;
        }
        if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(statement)) {
            emit_while_statement(*while_stmt);
            return;
        }
        if (const auto* for_stmt = dynamic_cast<const ForStmt*>(statement)) {
            emit_for_statement(*for_stmt);
            return;
        }
        if (dynamic_cast<const FallthroughStmt*>(statement) != nullptr) {
            return;
        }
        errors_.push_back(std::format("Unsupported statement kind '{}'", statement->kind()));
    }

    void emit_var_decl(const VarDeclStmt& variable) {
        SemanticType type = from_typeref(variable.type);
        if (variable.is_array) {
            type.is_array = true;
            type.array_size = variable.array_init.size();
            llvm::AllocaInst* slot = create_entry_alloca(current_function_, llvm_type(type), variable.name);
            declare_variable(variable.name, VariableInfo {type, slot, false});
            for (size_t i = 0; i < variable.array_init.size(); ++i) {
                llvm::Value* init_value = emit_expression(variable.array_init[i].get());
                SemanticType init_type = infer_expr_type(variable.array_init[i].get());
                init_value = cast_value(init_value, init_type, element_type(type), true);
                llvm::Value* element_address = builder_.CreateInBoundsGEP(llvm_type(type),
                                                                          slot,
                                                                          {builder_.getInt32(0), builder_.getInt32(static_cast<int>(i))},
                                                                          variable.name + ".elem");
                builder_.CreateStore(init_value, element_address);
            }
            return;
        }

        llvm::AllocaInst* slot = create_entry_alloca(current_function_, llvm_type(type), variable.name);
        declare_variable(variable.name, VariableInfo {type, slot, type_has_destructor(type)});
        if (variable.init) {
            llvm::Value* init_value = emit_expression(variable.init.get());
            SemanticType init_type = infer_expr_type(variable.init.get());
            builder_.CreateStore(cast_value(init_value, init_type, type, true), slot);
        } else {
            builder_.CreateStore(zero_value(type), slot);
        }
    }

    void emit_return(const ReturnStmt& return_stmt) {
        emit_all_scope_cleanups();
        if (return_stmt.value == nullptr) {
            builder_.CreateRetVoid();
            return;
        }
        llvm::Value* value = emit_expression(return_stmt.value.get());
        SemanticType actual_type = infer_expr_type(return_stmt.value.get());
        SemanticType expected_type = function_return_type(current_function_);
        value = cast_value(value, actual_type, expected_type, true);
        builder_.CreateRet(value);
    }

    SemanticType function_return_type(llvm::Function* function) const {
        for (const auto& info : owned_functions_) {
            if (info->llvm_function == function) {
                return info->return_type;
            }
        }
        return SemanticType {"void"};
    }

    void emit_yield(const YieldStmt& yield_stmt) {
        if (current_yield_ == nullptr) {
            errors_.push_back("'yield' used outside of expression block");
            return;
        }
        llvm::Value* value = emit_expression(yield_stmt.value.get());
        SemanticType actual_type = infer_expr_type(yield_stmt.value.get());
        builder_.CreateStore(cast_value(value, actual_type, current_yield_->type, true), current_yield_->slot);
        emit_scope_cleanups_until(current_yield_->scope_depth);
        builder_.CreateBr(current_yield_->merge_block);
    }

    void emit_delete(const DeleteStmt& delete_stmt) {
        llvm::Value* pointer = emit_expression(delete_stmt.value.get());
        SemanticType type = infer_expr_type(delete_stmt.value.get());
        if (pointer == nullptr || !type.is_pointer) {
            errors_.push_back("delete requires a pointer expression");
            return;
        }
        SemanticType pointee = type;
        pointee.is_pointer = false;
        pointee.is_reference = false;
        emit_destructor_call(pointee, pointer);
        builder_.CreateCall(ensure_free(), {builder_.CreateBitCast(pointer, llvm::PointerType::get(context_, 0))});
    }

    void emit_if_statement(const IfStmt& if_stmt) {
        llvm::Value* condition = emit_expression(if_stmt.condition.get());
        if (condition == nullptr) {
            return;
        }
        condition = as_boolean(condition, infer_expr_type(if_stmt.condition.get()));
        llvm::Function* function = current_function_;
        llvm::BasicBlock* then_block = llvm::BasicBlock::Create(context_, "if.then", function);
        llvm::BasicBlock* else_block = llvm::BasicBlock::Create(context_, "if.else", function);
        llvm::BasicBlock* merge_block = llvm::BasicBlock::Create(context_, "if.merge", function);
        builder_.CreateCondBr(condition, then_block, else_block);

        builder_.SetInsertPoint(then_block);
        emit_statement(if_stmt.then_stmt.get());
        if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            builder_.CreateBr(merge_block);
        }

        builder_.SetInsertPoint(else_block);
        emit_statement(if_stmt.else_stmt.get());
        if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            builder_.CreateBr(merge_block);
        }

        builder_.SetInsertPoint(merge_block);
    }

    void emit_while_statement(const WhileStmt& while_stmt) {
        llvm::Function* function = current_function_;
        llvm::BasicBlock* cond_block = llvm::BasicBlock::Create(context_, "while.cond", function);
        llvm::BasicBlock* body_block = llvm::BasicBlock::Create(context_, "while.body", function);
        llvm::BasicBlock* end_block = llvm::BasicBlock::Create(context_, "while.end", function);
        builder_.CreateBr(cond_block);

        builder_.SetInsertPoint(cond_block);
        llvm::Value* condition = emit_expression(while_stmt.condition.get());
        condition = as_boolean(condition, infer_expr_type(while_stmt.condition.get()));
        builder_.CreateCondBr(condition, body_block, end_block);

        builder_.SetInsertPoint(body_block);
        emit_statement(while_stmt.body.get());
        if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            builder_.CreateBr(cond_block);
        }

        builder_.SetInsertPoint(end_block);
    }

    void emit_for_statement(const ForStmt& for_stmt) {
        push_scope();
        if (for_stmt.range_var.has_value()) {
            emit_range_for(for_stmt);
            pop_scope();
            return;
        }

        emit_statement(for_stmt.init.get());
        llvm::Function* function = current_function_;
        llvm::BasicBlock* cond_block = llvm::BasicBlock::Create(context_, "for.cond", function);
        llvm::BasicBlock* body_block = llvm::BasicBlock::Create(context_, "for.body", function);
        llvm::BasicBlock* step_block = llvm::BasicBlock::Create(context_, "for.step", function);
        llvm::BasicBlock* end_block = llvm::BasicBlock::Create(context_, "for.end", function);
        builder_.CreateBr(cond_block);

        builder_.SetInsertPoint(cond_block);
        llvm::Value* condition = for_stmt.condition ? emit_expression(for_stmt.condition.get()) : llvm::ConstantInt::getTrue(context_);
        SemanticType condition_type = for_stmt.condition ? infer_expr_type(for_stmt.condition.get()) : SemanticType {"bool"};
        condition = as_boolean(condition, condition_type);
        builder_.CreateCondBr(condition, body_block, end_block);

        builder_.SetInsertPoint(body_block);
        emit_statement(for_stmt.body.get());
        if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            builder_.CreateBr(step_block);
        }

        builder_.SetInsertPoint(step_block);
        if (for_stmt.step) {
            emit_expression(for_stmt.step.get());
        }
        if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            builder_.CreateBr(cond_block);
        }

        builder_.SetInsertPoint(end_block);
        pop_scope();
    }

    void emit_range_for(const ForStmt& for_stmt) {
        const Parameter& range_var = *for_stmt.range_var;
        SemanticType range_type = infer_expr_type(for_stmt.range_expr.get());
        if (!range_type.is_array) {
            errors_.push_back("Range-for currently supports only local arrays");
            return;
        }

        SemanticType index_type = from_typeref(range_var.type);
        llvm::AllocaInst* index_slot = create_entry_alloca(current_function_, llvm_type(index_type), range_var.name);
        builder_.CreateStore(builder_.getInt32(0), index_slot);
        declare_variable(range_var.name, VariableInfo {index_type, index_slot, false});

        llvm::Function* function = current_function_;
        llvm::BasicBlock* cond_block = llvm::BasicBlock::Create(context_, "range.cond", function);
        llvm::BasicBlock* body_block = llvm::BasicBlock::Create(context_, "range.body", function);
        llvm::BasicBlock* step_block = llvm::BasicBlock::Create(context_, "range.step", function);
        llvm::BasicBlock* end_block = llvm::BasicBlock::Create(context_, "range.end", function);
        builder_.CreateBr(cond_block);

        builder_.SetInsertPoint(cond_block);
        llvm::Value* index_value = builder_.CreateLoad(llvm_type(index_type), index_slot, "range.idx");
        llvm::Value* limit = llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), range_type.array_size);
        llvm::Value* condition = builder_.CreateICmpSLT(index_value, limit, "range.has_next");
        builder_.CreateCondBr(condition, body_block, end_block);

        builder_.SetInsertPoint(body_block);
        emit_statement(for_stmt.body.get());
        if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            builder_.CreateBr(step_block);
        }

        builder_.SetInsertPoint(step_block);
        llvm::Value* next = builder_.CreateAdd(builder_.CreateLoad(llvm_type(index_type), index_slot), builder_.getInt32(1));
        builder_.CreateStore(next, index_slot);
        builder_.CreateBr(cond_block);

        builder_.SetInsertPoint(end_block);
    }
};

} // namespace

LLVMBackend::LLVMBackend(BackendOptions options)
    : options_(std::move(options)),
      context_(std::make_unique<llvm::LLVMContext>()) {}

LLVMBackend::~LLVMBackend() = default;

bool LLVMBackend::generate(const std::unordered_map<std::string, std::unique_ptr<frontend::TranslationUnit>>& units, std::ostream& err) {
    module_ = std::make_unique<llvm::Module>("dino", *context_);
    BackendImpl impl(*context_, *module_, err);
    return impl.generate(units);
}

bool LLVMBackend::write_ir(std::ostream& out, std::ostream& err) const {
    if (!module_) {
        err << "LLVM module was not generated\n";
        return false;
    }
    std::string ir;
    llvm::raw_string_ostream stream(ir);
    module_->print(stream, nullptr);
    stream.flush();

    if (options_.llvm_output_file.has_value()) {
        std::ofstream file(*options_.llvm_output_file, std::ios::binary);
        if (!file) {
            err << "Failed to open LLVM IR output file: " << *options_.llvm_output_file << "\n";
            return false;
        }
        file << ir;
        return true;
    }

    out << ir;
    return true;
}

bool LLVMBackend::write_object(const std::string& output_path, std::ostream& err) const {
    if (!module_) {
        err << "LLVM module was not generated\n";
        return false;
    }

    std::string target_error;
    const llvm::Triple triple(llvm::sys::getDefaultTargetTriple());
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, target_error);
    if (target == nullptr) {
        err << "Failed to initialize target '" << triple.str() << "': " << target_error << "\n";
        return false;
    }

    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(triple, "generic", "", options, std::nullopt));
    if (!machine) {
        err << "Failed to create target machine for object emission\n";
        return false;
    }

    module_->setTargetTriple(triple);
    module_->setDataLayout(machine->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(output_path, ec, llvm::sys::fs::OF_None);
    if (ec) {
        err << "Failed to open object output file: " << output_path << " (" << ec.message() << ")\n";
        return false;
    }

    llvm::legacy::PassManager pass_manager;
    if (machine->addPassesToEmitFile(pass_manager, dest, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        err << "Target machine cannot emit an object file for this target\n";
        return false;
    }

    pass_manager.run(*module_);
    dest.flush();
    return true;
}

} // namespace dino::codegen
