#include "dino/codegen/backend.hpp"

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
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/InitLLVM.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include "dino/frontend/parser.hpp"

namespace dino::codegen {
	namespace {

		using namespace dino::frontend;

		struct SemanticType {
			std::string name;
			bool is_const = false;
			bool is_nonull = false;
			int pointer_depth = 0;
			bool is_reference = false;
			bool is_array = false;
			size_t array_size = 0;
			bool is_error = false;

			[[nodiscard]] bool is_void() const { return !is_error && pointer_depth == 0 && !is_reference && !is_array && name == "void"; }
		};

		struct FieldInfo {
			std::string name;
			SemanticType type;
			size_t index = 0;
			bool is_static = false;
			const frontend::FieldDecl* decl = nullptr;
			llvm::GlobalVariable* global = nullptr;
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
			std::vector<bool> param_is_pack;
			std::vector<TemplateParam> template_params;
			bool variadic = false;
			bool external_only = false;
			bool is_extern = false;
			bool no_mangle = false;
			bool is_static_method = false;
			bool is_template_instance = false;
			bool is_defined = false;
			bool is_defining = false;
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
			std::unordered_map<std::string, FieldInfo> static_fields;
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

		struct StaticLocalInfo {
			SemanticType type;
			llvm::GlobalVariable* storage = nullptr;
			llvm::GlobalVariable* guard = nullptr;
			llvm::Function* cleanup = nullptr;
		};

		struct GlobalVarInfo {
			SemanticType type;
			bool is_extern = false;
			bool is_array = false;
			const GlobalVarDecl* decl = nullptr;
			llvm::GlobalVariable* global = nullptr;
		};

		struct PackElementInfo {
			SemanticType type;
			llvm::Value* address = nullptr;
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
			result.pointer_depth = type.pointer_depth;
			result.is_reference = type.is_reference;
			return result;
		}

		SemanticType from_typeref_string(const std::string& type_str) {
			SemanticType result;
			result.name = type_str;
			result.is_const = false;
			result.is_nonull = false;
			result.pointer_depth = 0;
			result.is_reference = false;
			result.is_array = false;
			result.array_size = 0;
			return result;
		}

		std::string template_param_key(const std::vector<TemplateParam>& params) {
			std::string key;
			for (const auto& param: params) {
				key += param.is_pack ? "..." : "";
				key += param.name;
				key += ";";
			}
			return key;
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
			for (int i = 0; i < type.pointer_depth; ++i) {
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
			if (type.is_error || type.pointer_depth > 0 || type.is_reference || type.is_array) {
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
			return !type.is_error && type.pointer_depth == 0 && !type.is_reference && !type.is_array &&
				   (type.name == "float" || type.name == "double");
		}

		bool is_numeric_type(const SemanticType& type) {
			return is_integer_type(type) || is_float_type(type);
		}

		bool is_bool_like(const SemanticType& type) {
			return type.name == "bool" || is_numeric_type(type) || type.pointer_depth > 0;
		}

		char decode_escape_char(char escaped) {
			switch (escaped) {
			case '0':
				return '\0';
			case 'a':
				return '\a';
			case 'b':
				return '\b';
			case 'f':
				return '\f';
			case 'n':
				return '\n';
			case 'r':
				return '\r';
			case 't':
				return '\t';
			case 'v':
				return '\v';
			case '\\':
				return '\\';
			case '\'':
				return '\'';
			case '"':
				return '"';
			default:
				return escaped;
			}
		}

		std::string unescape_literal(std::string_view escaped) {
			std::string decoded;
			decoded.reserve(escaped.size());
			for (size_t i = 0; i < escaped.size(); ++i) {
				if (escaped[i] == '\\' && i + 1 < escaped.size()) {
					decoded.push_back(decode_escape_char(escaped[++i]));
					continue;
				}
				decoded.push_back(escaped[i]);
			}
			return decoded;
		}

		bool same_type(const SemanticType& lhs, const SemanticType& rhs) {
			return lhs.name == rhs.name && lhs.pointer_depth == rhs.pointer_depth && lhs.is_reference == rhs.is_reference &&
				   lhs.is_array == rhs.is_array && lhs.array_size == rhs.array_size;
		}

		bool matches_template_wrapper_shape(const SemanticType& pattern, const SemanticType& actual) {
			if (pattern.pointer_depth > 0 && actual.pointer_depth == 0) {
				return false;
			}
			if (pattern.is_reference && !actual.is_reference) {
				return false;
			}
			if (pattern.is_array && !actual.is_array) {
				return false;
			}
			return true;
		}

		SemanticType numeric_common_type(const SemanticType& lhs, const SemanticType& rhs) {
			if (lhs.name == "double" || rhs.name == "double") {
				return SemanticType{"double"};
			}
			if (lhs.name == "float" || rhs.name == "float") {
				return SemanticType{"float"};
			}
			if (lhs.name == "uint64" || rhs.name == "uint64" || lhs.name == "int64" || rhs.name == "int64") {
				return SemanticType{"int64"};
			}
			return SemanticType{"int32"};
		}

		bool is_assignable_to(const SemanticType& from, const SemanticType& to) {
			if (same_type(from, to)) {
				return true;
			}
			if (is_numeric_type(from) && is_numeric_type(to)) {
				return true;
			}
			if (from.is_array && to.pointer_depth > 0 && from.name == to.name) {
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
				declare_static_fields();
				declare_globals();
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
				// Second pass: define functions for template structs instantiated during code generation
				define_template_struct_functions();
				if (!errors_.empty()) {
					flush_errors();
					return false;
				}
				// Third pass: define all template function instances
				define_template_function_instances();
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

			struct SavedState {
				llvm::BasicBlock* insert_block = nullptr;
				const TranslationUnit* current_unit = nullptr;
				const StructInfo* current_struct = nullptr;
				llvm::Value* current_self = nullptr;
				llvm::Function* current_function = nullptr;
				YieldContext* current_yield = nullptr;
				std::vector<Scope> scopes;
				std::unordered_map<std::string, SemanticType> template_bindings;
				std::unordered_map<std::string, std::vector<PackElementInfo>> pack_bindings;
			};

			llvm::LLVMContext& context_;
			llvm::Module& module_;
			llvm::IRBuilder<> builder_;
			std::ostream& err_;
			const std::unordered_map<std::string, std::unique_ptr<TranslationUnit>>* units_ = nullptr;
			std::unordered_map<std::string, StructInfo> structs_;
			std::unordered_map<std::string, StructInfo> template_structs_;
			std::unordered_map<std::string, std::vector<FunctionInfo*>> free_functions_;
			std::unordered_map<std::string, std::vector<FunctionInfo*>> template_free_functions_;
			std::unordered_map<std::string, GlobalVarInfo> globals_;
			std::unordered_map<std::string, FunctionInfo*> template_instances_;
			std::vector<std::unique_ptr<FunctionInfo>> owned_functions_;
			std::vector<std::string> errors_;
			std::unique_ptr<llvm::TargetMachine> target_machine_;
			std::vector<Scope> scopes_;
			const TranslationUnit* current_unit_ = nullptr;
			const StructInfo* current_struct_ = nullptr;
			llvm::Value* current_self_ = nullptr;
			llvm::Function* current_function_ = nullptr;
			YieldContext* current_yield_ = nullptr;
			std::unordered_map<std::string, SemanticType> current_template_bindings_;
			std::unordered_map<std::string, std::vector<PackElementInfo>> current_pack_bindings_;
			std::unordered_map<std::string, StaticLocalInfo> static_locals_;
			size_t static_local_id_ = 0;
			unsigned string_id_ = 0;

			SavedState save_state() const {
				return SavedState{
					builder_.GetInsertBlock(),
					current_unit_,
					current_struct_,
					current_self_,
					current_function_,
					current_yield_,
					scopes_,
					current_template_bindings_,
					current_pack_bindings_};
			}

			void restore_state(SavedState&& state) {
				if (state.insert_block != nullptr) {
					builder_.SetInsertPoint(state.insert_block);
				}
				current_unit_ = state.current_unit;
				current_struct_ = state.current_struct;
				current_self_ = state.current_self;
				current_function_ = state.current_function;
				current_yield_ = state.current_yield;
				scopes_ = std::move(state.scopes);
				current_template_bindings_ = std::move(state.template_bindings);
				current_pack_bindings_ = std::move(state.pack_bindings);
			}

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
				for (const auto& error: errors_) {
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

			void declare_variable(const std::string& name, VariableInfo variable, bool track_cleanup = true) {
				if (scopes_.empty()) {
					push_scope();
				}
				if (track_cleanup) {
					scopes_.back().destruction_order.push_back(name);
				}
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

			llvm::Function* ensure_atexit() {
				if (llvm::Function* fn = module_.getFunction("atexit")) {
					return fn;
				}
				llvm::FunctionType* type = llvm::FunctionType::get(llvm::Type::getInt32Ty(context_),
																   {llvm::PointerType::get(context_, 0)},
																   false);
				return llvm::Function::Create(type, llvm::Function::ExternalLinkage, "atexit", module_);
			}

			llvm::StructType* heap_header_type() {
				llvm::StructType* header = llvm::StructType::getTypeByName(context_, "dino.heap.header");
				if (header != nullptr) {
					return header;
				}
				header = llvm::StructType::create(context_, "dino.heap.header");
				header->setBody({llvm::Type::getInt64Ty(context_)}, false);
				return header;
			}

			bool type_has_destructor(const SemanticType& type) const {
				if (type.pointer_depth > 0 || type.is_reference || type.is_array) {
					return false;
				}
				const auto found = structs_.find(type.name);
				return found != structs_.end() && found->second.destructor != nullptr;
			}

			bool type_needs_static_cleanup(const SemanticType& type) const {
				if (type.pointer_depth > 0 || type.is_reference) {
					return false;
				}
				if (type.is_array) {
					return type_has_destructor(element_type(type));
				}
				return type_has_destructor(type);
			}

			void emit_destructor_call(const SemanticType& type, llvm::Value* address) {
				if (address == nullptr || !type_has_destructor(type)) {
					return;
				}
				const auto it = structs_.find(type.name);
				const auto template_it = template_structs_.find(type.name);
				if (it == structs_.end() && template_it == template_structs_.end()) {
					errors_.push_back(std::format("Cannot find struct '{}' for destructor call", type.name));
					return;
				}
				const StructInfo& info = (it != structs_.end()) ? it->second : template_it->second;
				builder_.CreateCall(info.destructor->llvm_function, {address});
			}

			void emit_static_cleanup_call(const SemanticType& type, llvm::Value* address) {
				if (address == nullptr) {
					return;
				}
				if (type.is_array) {
					SemanticType element = element_type(type);
					if (!type_has_destructor(element)) {
						return;
					}

					llvm::Function* function = current_function_;
					llvm::BasicBlock* cond_block = llvm::BasicBlock::Create(context_, "static.array.dtor.cond", function);
					llvm::BasicBlock* body_block = llvm::BasicBlock::Create(context_, "static.array.dtor.body", function);
					llvm::BasicBlock* end_block = llvm::BasicBlock::Create(context_, "static.array.dtor.end", function);
					llvm::AllocaInst* index_slot = create_entry_alloca(function, llvm::Type::getInt64Ty(context_), "static.dtor.index");
					builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), type.array_size), index_slot);
					builder_.CreateBr(cond_block);

					builder_.SetInsertPoint(cond_block);
					llvm::Value* index = builder_.CreateLoad(llvm::Type::getInt64Ty(context_), index_slot, "static.dtor.index");
					llvm::Value* has_more = builder_.CreateICmpUGT(index,
																   llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0),
																   "static.dtor.has_more");
					builder_.CreateCondBr(has_more, body_block, end_block);

					builder_.SetInsertPoint(body_block);
					llvm::Value* current = builder_.CreateSub(index,
															  llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1),
															  "static.dtor.current");
					llvm::Value* element_address = builder_.CreateInBoundsGEP(llvm_type(type), address, {builder_.getInt32(0), current}, "static.dtor.elem");
					emit_destructor_call(element, element_address);
					builder_.CreateStore(current, index_slot);
					builder_.CreateBr(cond_block);

					builder_.SetInsertPoint(end_block);
					return;
				}

				emit_destructor_call(type, address);
			}

			llvm::Value* allocate_heap_block(const SemanticType& allocated_type, llvm::Value* element_count) {
				llvm::Type* element_llvm_type = llvm_type(allocated_type);
				const llvm::DataLayout& layout = module_.getDataLayout();
				const uint64_t element_size = layout.getTypeAllocSize(element_llvm_type);
				const uint64_t header_size = layout.getTypeAllocSize(heap_header_type());

				llvm::Value* count64 = builder_.CreateIntCast(element_count, llvm::Type::getInt64Ty(context_), false, "heap.count");
				llvm::Value* bytes = builder_.CreateMul(count64,
														llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), element_size),
														"heap.payload.bytes");
				bytes = builder_.CreateAdd(bytes,
										   llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), header_size),
										   "heap.total.bytes");

				llvm::Value* raw = builder_.CreateCall(ensure_malloc(), {bytes}, "new.raw");
				llvm::Value* header_ptr = builder_.CreateBitCast(raw, llvm::PointerType::get(context_, 0), "heap.header.ptr");
				llvm::Value* count_ptr = builder_.CreateStructGEP(heap_header_type(), header_ptr, 0, "heap.count.ptr");
				builder_.CreateStore(count64, count_ptr);

				llvm::Value* payload_raw = builder_.CreateInBoundsGEP(llvm::Type::getInt8Ty(context_),
																	  builder_.CreateBitCast(raw, llvm::PointerType::get(context_, 0)),
																	  llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), header_size),
																	  "heap.payload.raw");
				return builder_.CreateBitCast(payload_raw, llvm::PointerType::get(context_, 0), "new.ptr");
			}

			llvm::Value* heap_count_from_payload(llvm::Value* payload) {
				const llvm::DataLayout& layout = module_.getDataLayout();
				const uint64_t header_size = layout.getTypeAllocSize(heap_header_type());
				llvm::Value* payload_raw = builder_.CreateBitCast(payload, llvm::PointerType::get(context_, 0), "payload.raw");
				llvm::Value* header_raw = builder_.CreateInBoundsGEP(llvm::Type::getInt8Ty(context_),
																	 payload_raw,
																	 llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), -static_cast<int64_t>(header_size)),
																	 "header.raw");
				llvm::Value* header_ptr = builder_.CreateBitCast(header_raw, llvm::PointerType::get(context_, 0), "header.ptr");
				llvm::Value* count_ptr = builder_.CreateStructGEP(heap_header_type(), header_ptr, 0, "heap.count.ptr");
				return builder_.CreateLoad(llvm::Type::getInt64Ty(context_), count_ptr, "heap.count");
			}

			llvm::Value* heap_raw_from_payload(llvm::Value* payload) {
				const llvm::DataLayout& layout = module_.getDataLayout();
				const uint64_t header_size = layout.getTypeAllocSize(heap_header_type());
				llvm::Value* payload_raw = builder_.CreateBitCast(payload, llvm::PointerType::get(context_, 0), "payload.raw");
				return builder_.CreateInBoundsGEP(llvm::Type::getInt8Ty(context_),
												  payload_raw,
												  llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), -static_cast<int64_t>(header_size)),
												  "heap.raw");
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
				for (const auto& [path, unit]: *units_) {
					(void)path;
					for (const auto& decl: unit->declarations) {
						if (const auto* struct_decl = dynamic_cast<const StructDecl*>(decl.get())) {
							StructInfo info;
							info.decl = struct_decl;
							size_t field_index = 0;
							for (const auto& field: struct_decl->fields) {
								for (const auto& name: field.names) {
									FieldInfo field_info;
									field_info.name = name;
									field_info.type = from_typeref(field.type);
									field_info.is_static = field.is_static;
									field_info.decl = &field;
									if (field.is_static) {
										info.static_fields[name] = field_info;
									} else {
										field_info.index = field_index++;
										info.field_indices[name] = info.fields.size();
										info.fields.push_back(field_info);
									}
								}
							}
							// Store template structs in template_structs_, regular structs in structs_
							if (!struct_decl->template_params.empty()) {
								template_structs_[struct_decl->name] = std::move(info);
							} else {
								structs_[struct_decl->name] = std::move(info);
							}
						}
					}
				}

				for (const auto& [path, unit]: *units_) {
					(void)path;
					for (const auto& decl: unit->declarations) {
						if (const auto* function_decl = dynamic_cast<const FunctionDecl*>(decl.get())) {
							auto info = std::make_unique<FunctionInfo>();
							info->kind = FunctionKind::Free;
							info->name = function_decl->name;
							info->return_type = from_typeref(function_decl->return_type);
							info->owner_unit = unit.get();
							info->function = function_decl;
							info->external_only = function_decl->body == nullptr;
							info->is_extern = function_decl->attributes.is_extern;
							info->no_mangle = function_decl->attributes.no_mangle;
							info->template_params = function_decl->template_params;
							for (const auto& parameter: function_decl->parameters) {
								if (parameter.type.variadic) {
									info->variadic = true;
									continue;
								}
								SemanticType param_type = from_typeref(parameter.type);
								info->param_is_pack.push_back(parameter.is_pack);
								info->params.push_back(param_type);
							}
							if (function_decl->template_params.empty()) {
								info->llvm_name = mangle_free_function(*info);
								free_functions_[info->name].push_back(info.get());
							} else {
								template_free_functions_[info->name].push_back(info.get());
							}
							owned_functions_.push_back(std::move(info));
							continue;
						}

						if (const auto* global_decl = dynamic_cast<const GlobalVarDecl*>(decl.get())) {
							GlobalVarInfo info;
							info.type = from_typeref(global_decl->type);
							info.type.is_array = global_decl->is_array;
							if (global_decl->is_array) {
								info.type.array_size = global_decl->array_init.size();
							}
							info.is_extern = global_decl->is_extern;
							info.is_array = global_decl->is_array;
							info.decl = global_decl;
							globals_[global_decl->name] = std::move(info);
							continue;
						}

						const auto* struct_decl = dynamic_cast<const StructDecl*>(decl.get());
						if (struct_decl == nullptr) {
							continue;
						}
						// Look in both structs_ and template_structs_ for the struct
						StructInfo* owner_ptr = nullptr;
						if (const auto it = structs_.find(struct_decl->name); it != structs_.end()) {
							owner_ptr = &it->second;
						} else if (const auto it = template_structs_.find(struct_decl->name); it != template_structs_.end()) {
							owner_ptr = &it->second;
						}
						if (owner_ptr == nullptr) {
							continue;
						}
						StructInfo& owner = *owner_ptr;
						for (const auto& constructor_decl: struct_decl->constructors) {
							auto info = std::make_unique<FunctionInfo>();
							info->kind = FunctionKind::Constructor;
							info->owner = struct_decl->name;
							info->name = struct_decl->name;
							// Constructors return void (they modify struct in-place via this pointer)
							info->return_type = SemanticType{"void"};
							info->owner_unit = unit.get();
							info->constructor = &constructor_decl;
							for (const auto& parameter: constructor_decl.parameters) {
								if (parameter.type.variadic) {
									info->variadic = true;
									info->external_only = true;
									continue;
								}
								info->param_is_pack.push_back(parameter.is_pack);
								info->params.push_back(from_typeref(parameter.type));
							}
							info->llvm_name = mangle_constructor(*info);
							owner.constructors.push_back(info.get());
							owned_functions_.push_back(std::move(info));
						}
						for (const auto& method_decl: struct_decl->methods) {
							auto info = std::make_unique<FunctionInfo>();
							info->kind = FunctionKind::Method;
							info->owner = struct_decl->name;
							info->name = method_decl.name;
							info->return_type = from_typeref(method_decl.return_type);
							info->owner_unit = unit.get();
							info->method = &method_decl;
							info->external_only = method_decl.body == nullptr;
							info->is_extern = method_decl.attributes.is_extern;
							info->no_mangle = method_decl.attributes.no_mangle;
							info->is_static_method = method_decl.is_static;
							info->template_params = method_decl.template_params;
							for (const auto& parameter: method_decl.parameters) {
								if (parameter.type.variadic) {
									info->variadic = true;
									continue;
								}
								info->param_is_pack.push_back(parameter.is_pack);
								info->params.push_back(from_typeref(parameter.type));
							}
							info->llvm_name = mangle_method(*info);
							owner.methods[method_decl.name].push_back(info.get());
							owned_functions_.push_back(std::move(info));
						}
						for (const auto& destructor_decl: struct_decl->destructors) {
							auto info = std::make_unique<FunctionInfo>();
							info->kind = FunctionKind::Destructor;
							info->owner = struct_decl->name;
							info->name = destructor_decl.name;
							info->return_type = SemanticType{"void"};
							info->owner_unit = unit.get();
							info->destructor = &destructor_decl;
							info->llvm_name = mangle_destructor(*info);
							owner.destructor = info.get();
							owned_functions_.push_back(std::move(info));
						}
						for (const auto& conversion_decl: struct_decl->conversions) {
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
				if (function.is_extern || function.no_mangle) {
					return function.name;
				}
				return std::format("{} {}({})", type_to_string(function.return_type), function.name, join_params(function.params));
			}

			std::string mangle_method(const FunctionInfo& function) const {
				if (function.is_extern || function.no_mangle) {
					return std::format("{}_{}", function.owner, function.name);
				}
				std::vector<SemanticType> mangled_params;
				if (!function.is_static_method) {
					SemanticType self_type{function.owner};
					self_type.pointer_depth = 1;
					mangled_params.push_back(self_type);
				}
				mangled_params.insert(mangled_params.end(), function.params.begin(), function.params.end());
				return std::format("method {}.{}({})", function.owner, function.name, join_params(mangled_params));
			}

			std::string mangle_constructor(const FunctionInfo& function) const {
				return std::format("ctor {} {}({})", function.owner, function.name, join_params(function.params));
			}

			std::string mangle_destructor(const FunctionInfo& function) const {
				return std::format("dtor {} ~{}()", function.owner, function.owner);
			}

			std::string mangle_conversion(const FunctionInfo& function) const {
				return std::format("conv {} {}()", type_to_string(function.return_type), function.owner);
			}

			std::optional<std::pair<std::string, std::vector<SemanticType>>> parse_template_instantiation(const std::string& type_name) const {
				size_t lt_pos = type_name.find('<');
				size_t gt_pos = type_name.rfind('>');
				if (lt_pos == std::string::npos || gt_pos == std::string::npos || gt_pos <= lt_pos) {
					return std::nullopt;
				}
				std::string base_name = type_name.substr(0, lt_pos);
				std::string args_str = type_name.substr(lt_pos + 1, gt_pos - lt_pos - 1);
				std::vector<SemanticType> args;
				size_t pos = 0;
				while (pos < args_str.size()) {
					size_t comma_pos = args_str.find(',', pos);
					if (comma_pos == std::string::npos) {
						comma_pos = args_str.size();
					}
					std::string arg_name = args_str.substr(pos, comma_pos - pos);
					SemanticType arg;
					arg.name = arg_name;
					args.push_back(arg);
					pos = comma_pos + 1;
				}
				return std::make_pair(base_name, args);
			}

			llvm::Type* llvm_type(const SemanticType& type, bool decay_array = false) {
				// Array types must be checked first: an array of pointers (e.g.
				// `const char* x[]`) has both pointer_depth > 0 *and* is_array,
				// but its underlying LLVM type is `[N x ptr]`, not `ptr`.
				if (type.is_array) {
					if (decay_array) {
						return llvm::PointerType::get(context_, 0);
					}
					return llvm::ArrayType::get(llvm_type(element_type(type)), type.array_size);
				}
				// Handle void* specially - it's just a pointer type
				if (type.name == "void" && type.pointer_depth > 0) {
					return llvm::PointerType::get(context_, 0);
				}
				if (type.is_reference || type.pointer_depth > 0) {
					return llvm::PointerType::get(context_, 0);
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
				// Also check template_structs_ for template definitions
				// This is needed when processing function signatures that reference the template name
				if (const auto found = template_structs_.find(type.name); found != template_structs_.end()) {
					// For template definitions, return an opaque type that will be defined when instantiated
					// This allows us to process template function signatures
					if (found->second.llvm_type == nullptr) {
						found->second.llvm_type = llvm::StructType::create(context_, type.name);
					}
					return found->second.llvm_type;
				}
				// Handle template instantiation types like "Array<int32>"
				if (const auto parsed = parse_template_instantiation(type.name)) {
					const auto& [base_name, args] = *parsed;
					// Look in both structs_ and template_structs_ for the base template
					const auto base_struct = structs_.find(base_name);
					const auto base_template = template_structs_.find(base_name);
					const StructInfo* base_info = nullptr;
					if (base_struct != structs_.end()) {
						base_info = &base_struct->second;
					} else if (base_template != template_structs_.end()) {
						base_info = &base_template->second;
					}
					if (base_info != nullptr) {
						// Check if this instantiation already exists
						if (const auto inst_struct = structs_.find(type.name); inst_struct != structs_.end()) {
							return inst_struct->second.llvm_type;
						}
						// Create new struct instance
						StructInfo inst_struct;
						inst_struct.decl = base_info->decl;
						inst_struct.llvm_type = llvm::StructType::create(context_, type.name);
						// Substitute template parameters in fields
						std::unordered_map<std::string, SemanticType> bindings;
						const auto& template_params = base_info->decl->template_params;
						if (template_params.size() != args.size()) {
							errors_.push_back(std::format("Template parameter count mismatch for '{}': expected {}, got {}", type.name, template_params.size(), args.size()));
							return llvm::Type::getVoidTy(context_);
						}
						for (size_t i = 0; i < template_params.size(); ++i) {
							bindings[template_params[i].name] = args[i];
						}
						for (const auto& field: base_info->fields) {
							FieldInfo inst_field = field;
							inst_field.type = substitute_semantic_type(field.type, bindings);
							inst_struct.fields.push_back(inst_field);
							inst_struct.field_indices[field.name] = inst_struct.fields.size() - 1;
						}
						// Instantiate constructors, methods, and destructor
						for (FunctionInfo* constructor: base_info->constructors) {
							if (!constructor->template_params.empty()) {
								std::vector<SemanticType> ctor_args;
								for (const auto& param: constructor->params) {
									ctor_args.push_back(substitute_semantic_type(param, bindings));
								}
								if (FunctionInfo* inst_ctor = instantiate_template_function(constructor, ctor_args)) {
									// Update the owner to the instantiated struct name
									inst_ctor->owner = type.name;
									inst_struct.constructors.push_back(inst_ctor);
								}
							} else {
								// For non-template constructors, create a copy with updated owner and types
								auto ctor_copy = std::make_unique<FunctionInfo>(*constructor);
								ctor_copy->owner = type.name;
								// Mark as template instance so define_constructor sets up template bindings
								ctor_copy->is_template_instance = true;
								// Substitute types in parameters and return type
								for (auto& param: ctor_copy->params) {
									param = substitute_semantic_type(param, bindings);
								}
								ctor_copy->return_type = substitute_semantic_type(constructor->return_type, bindings);
								ctor_copy->llvm_name = mangle_constructor(*ctor_copy);
								// Create LLVM function for the constructor
								std::vector<llvm::Type*> llvm_params;
								llvm_params.push_back(llvm::PointerType::get(context_, 0)); // this pointer
								for (const auto& parameter: ctor_copy->params) {
									llvm_params.push_back(llvm_type(parameter, true));
								}
								llvm::FunctionType* function_type =
									llvm::FunctionType::get(llvm_type(ctor_copy->return_type), llvm_params, ctor_copy->variadic);
								ctor_copy->llvm_function = llvm::Function::Create(function_type,
																				 llvm::Function::ExternalLinkage,
																				 ctor_copy->llvm_name,
																				 module_);
								FunctionInfo* ctor_ptr = ctor_copy.get();
								owned_functions_.push_back(std::move(ctor_copy));
								inst_struct.constructors.push_back(ctor_ptr);
							}
						}
						for (const auto& [method_name, methods]: base_info->methods) {
							for (FunctionInfo* method: methods) {
								// Skip variadic template methods for now - they're not used in the current instantiation
								bool has_variadic_template = false;
								for (const auto& tp: method->template_params) {
									if (tp.is_pack) {
										has_variadic_template = true;
										break;
									}
								}
								if (has_variadic_template) {
									// Don't instantiate variadic template methods at struct instantiation time
									// They will be instantiated when called
									continue;
								}
								if (!method->template_params.empty()) {
									std::vector<SemanticType> method_args;
									for (const auto& param: method->params) {
										method_args.push_back(substitute_semantic_type(param, bindings));
									}
									if (FunctionInfo* inst_method = instantiate_template_function(method, method_args)) {
										// Update the owner to the instantiated struct name
										inst_method->owner = type.name;
										inst_method->is_template_instance = true;
										inst_struct.methods[method_name].push_back(inst_method);
									}
								} else {
									// For non-template methods, create a copy with updated owner and types
									auto method_copy = std::make_unique<FunctionInfo>(*method);
									method_copy->owner = type.name;
									// Mark as template instance so define_method sets up template bindings
									method_copy->is_template_instance = true;
									// Substitute types in parameters and return type
									for (auto& param: method_copy->params) {
										param = substitute_semantic_type(param, bindings);
									}
									method_copy->return_type = substitute_semantic_type(method->return_type, bindings);
									method_copy->llvm_name = mangle_method(*method_copy);
									// Create LLVM function for the method
									std::vector<llvm::Type*> llvm_params;
									if (!method_copy->is_static_method) {
										llvm_params.push_back(llvm::PointerType::get(context_, 0)); // this pointer
									}
									for (const auto& parameter: method_copy->params) {
										llvm_params.push_back(llvm_type(parameter, true));
									}
									llvm::FunctionType* function_type =
										llvm::FunctionType::get(llvm_type(method_copy->return_type), llvm_params, method_copy->variadic);
									method_copy->llvm_function = llvm::Function::Create(function_type,
																						llvm::Function::ExternalLinkage,
																						method_copy->llvm_name,
																						module_);
									FunctionInfo* method_ptr = method_copy.get();
									owned_functions_.push_back(std::move(method_copy));
									inst_struct.methods[method_name].push_back(method_ptr);
								}
							}
						}
						if (base_info->destructor != nullptr && !base_info->destructor->template_params.empty()) {
							if (FunctionInfo* inst_dtor = instantiate_template_function(base_info->destructor, {})) {
								// Update the owner to the instantiated struct name
								inst_dtor->owner = type.name;
								inst_struct.destructor = inst_dtor;
							}
						} else if (base_info->destructor != nullptr) {
							// For non-template destructor, create a copy with updated owner and types
							auto dtor_copy = std::make_unique<FunctionInfo>(*base_info->destructor);
							dtor_copy->owner = type.name;
							// Mark as template instance so define_destructor sets up template bindings
							dtor_copy->is_template_instance = true;
							// Substitute types in parameters and return type
							for (auto& param: dtor_copy->params) {
								param = substitute_semantic_type(param, bindings);
							}
							dtor_copy->return_type = substitute_semantic_type(base_info->destructor->return_type, bindings);
							dtor_copy->llvm_name = mangle_destructor(*dtor_copy);
							// Create LLVM function for the destructor
							std::vector<llvm::Type*> llvm_params;
							llvm_params.push_back(llvm::PointerType::get(context_, 0)); // this pointer
							for (const auto& parameter: dtor_copy->params) {
								llvm_params.push_back(llvm_type(parameter, true));
							}
							llvm::FunctionType* function_type =
								llvm::FunctionType::get(llvm_type(dtor_copy->return_type), llvm_params, dtor_copy->variadic);
							dtor_copy->llvm_function = llvm::Function::Create(function_type,
																				 llvm::Function::ExternalLinkage,
																				 dtor_copy->llvm_name,
																				 module_);
							FunctionInfo* dtor_ptr = dtor_copy.get();
							owned_functions_.push_back(std::move(dtor_copy));
							inst_struct.destructor = dtor_ptr;
						}
						// Define the struct body immediately
						std::vector<llvm::Type*> field_types;
						field_types.reserve(inst_struct.fields.size());
						for (const auto& field: inst_struct.fields) {
							llvm::Type* field_type = llvm_type(field.type);
							if (field_type == nullptr) {
								errors_.push_back(std::format("Failed to get LLVM type for field '{}' in template instantiation '{}'", field.name, type.name));
								return llvm::Type::getVoidTy(context_);
							}
							field_types.push_back(field_type);
						}
						inst_struct.llvm_type->setBody(field_types, false);
						structs_[type.name] = std::move(inst_struct);
						return structs_[type.name].llvm_type;
					}
				}
				errors_.push_back(std::format("Unknown Dino type '{}'", type_to_string(type)));
				return llvm::Type::getVoidTy(context_);
			}

			SemanticType element_type(const SemanticType& type) const {
				SemanticType result = type;
				if (type.is_array) {
					// Stripping the array dimension preserves the underlying element
					// type — including any pointer depth (e.g. element of `[N x T*]`
					// is `T*`, not `T`).
					result.is_array = false;
					result.array_size = 0;
					return result;
				}
				// Pointer/reference dereference: peel off one level.
				if (result.pointer_depth > 0) {
					--result.pointer_depth;
				}
				result.is_reference = false;
				return result;
			}

			SemanticType substitute_typeref(const TypeRef& type) const {
				SemanticType result = from_typeref(type);
				if (const auto found = current_template_bindings_.find(result.name); found != current_template_bindings_.end()) {
					SemanticType substituted = found->second;
					substituted.is_const = substituted.is_const || result.is_const;
					substituted.is_nonull = substituted.is_nonull || result.is_nonull;
					substituted.pointer_depth = std::max(substituted.pointer_depth, result.pointer_depth);
					substituted.is_reference = substituted.is_reference || result.is_reference;
					substituted.is_array = substituted.is_array || result.is_array;
					if (result.is_array && result.array_size != 0) {
						substituted.array_size = result.array_size;
					}
					return substituted;
				}
				// Check if this is a template instantiation that needs to be resolved
				size_t lt_pos = result.name.find('<');
				size_t gt_pos = result.name.rfind('>');
				if (lt_pos != std::string::npos && gt_pos != std::string::npos && gt_pos > lt_pos) {
					// This is a template instantiation like Array<int32>
					// Return it as-is - llvm_type will handle the instantiation
					return result;
				}
				// Check if this is a base template name that should be substituted with the instantiated name
				// For example, "Array" should become "Array<int32>" when inside an Array<int32> method
				for (const auto& [base_name, inst_type]: current_template_bindings_) {
					// Skip if this is a template parameter binding (like ElementType -> int32)
					if (base_name.find('<') == std::string::npos && result.name == base_name) {
						// This is the base template name, substitute with instantiated name
						SemanticType substituted = inst_type;
						substituted.is_const = substituted.is_const || result.is_const;
						substituted.is_nonull = substituted.is_nonull || result.is_nonull;
						substituted.pointer_depth = std::max(substituted.pointer_depth, result.pointer_depth);
						substituted.is_reference = substituted.is_reference || result.is_reference;
						substituted.is_array = substituted.is_array || result.is_array;
						if (result.is_array && result.array_size != 0) {
							substituted.array_size = result.array_size;
						}
						return substituted;
					}
				}
				return result;
			}

			bool deduce_template_binding(const SemanticType& pattern,
										 const SemanticType& actual,
										 const std::unordered_map<std::string, bool>& template_params,
										 std::unordered_map<std::string, SemanticType>& bindings) const {
				if (const auto found = template_params.find(pattern.name); found != template_params.end()) {
					if (!matches_template_wrapper_shape(pattern, actual)) {
						return false;
					}
					SemanticType deduced = actual;
					if (pattern.pointer_depth > 0) {
						deduced.pointer_depth = std::max(0, deduced.pointer_depth - pattern.pointer_depth);
					}
					if (pattern.is_reference) {
						deduced.is_reference = false;
					}
					if (pattern.is_array) {
						deduced.is_array = false;
						deduced.array_size = 0;
					}
					if (const auto bound = bindings.find(pattern.name); bound != bindings.end()) {
						return same_type(bound->second, deduced);
					}
					bindings[pattern.name] = deduced;
					return true;
				}
				return is_assignable_to(actual, pattern) || same_type(actual, pattern);
			}

			SemanticType substitute_semantic_type(const SemanticType& type,
												  const std::unordered_map<std::string, SemanticType>& bindings) const {
				if (const auto found = bindings.find(type.name); found != bindings.end()) {
					SemanticType substituted = found->second;
					substituted.is_const = substituted.is_const || type.is_const;
					substituted.is_nonull = substituted.is_nonull || type.is_nonull;
					substituted.pointer_depth = std::max(substituted.pointer_depth, type.pointer_depth);
					substituted.is_reference = substituted.is_reference || type.is_reference;
					substituted.is_array = substituted.is_array || type.is_array;
					if (type.is_array && type.array_size != 0) {
						substituted.array_size = type.array_size;
					}
					return substituted;
				}
				return type;
			}

			std::string specialization_key(const FunctionInfo& function,
										   const std::vector<SemanticType>& params,
										   const std::unordered_map<std::string, SemanticType>& bindings) const {
				std::string key = (function.owner.empty() ? function.name : function.owner + "::" + function.name) +
								  "<" + template_param_key(function.template_params) + ">(";
				for (size_t i = 0; i < params.size(); ++i) {
					if (i != 0) {
						key += ",";
					}
					key += type_to_string(params[i]);
				}
				key += "){";
				for (const auto& param: function.template_params) {
					if (const auto found = bindings.find(param.name); found != bindings.end()) {
						key += param.name + "=" + type_to_string(found->second) + ";";
					}
				}
				key += "}";
				return key;
			}

			FunctionInfo* instantiate_template_function(FunctionInfo* function_template, const std::vector<SemanticType>& args) {
				if (function_template == nullptr ||
					(function_template->function == nullptr && function_template->method == nullptr &&
					 function_template->constructor == nullptr && function_template->destructor == nullptr) ||
					function_template->template_params.empty()) {
					return nullptr;
				}

				std::unordered_map<std::string, bool> template_params;
				std::unordered_map<std::string, bool> pack_template_params;
				for (const auto& param: function_template->template_params) {
					template_params[param.name] = param.is_pack;
					if (param.is_pack) {
						pack_template_params[param.name] = true;
					}
				}

				const bool has_pack = std::ranges::any_of(function_template->param_is_pack, [](bool value) { return value; });
				std::unordered_map<std::string, SemanticType> bindings;
				std::vector<SemanticType> concrete_params;
				if (has_pack) {
					size_t pack_index = 0;
					while (pack_index < function_template->param_is_pack.size() && !function_template->param_is_pack[pack_index]) {
						++pack_index;
					}
					if (pack_index != function_template->param_is_pack.size() - 1 || args.size() < pack_index) {
						return nullptr;
					}
					for (size_t i = 0; i < pack_index; ++i) {
						if (!deduce_template_binding(function_template->params[i], args[i], template_params, bindings)) {
							return nullptr;
						}
						concrete_params.push_back(substitute_semantic_type(function_template->params[i], bindings));
					}
					for (size_t i = pack_index; i < args.size(); ++i) {
						if (pack_template_params.contains(function_template->params[pack_index].name)) {
							if (!matches_template_wrapper_shape(function_template->params[pack_index], args[i])) {
								return nullptr;
							}
							concrete_params.push_back(args[i]);
							continue;
						}
						if (!deduce_template_binding(function_template->params[pack_index], args[i], template_params, bindings)) {
							return nullptr;
						}
						concrete_params.push_back(args[i]);
					}
				} else {
					if (args.size() != function_template->params.size()) {
						return nullptr;
					}
					for (size_t i = 0; i < args.size(); ++i) {
						if (!deduce_template_binding(function_template->params[i], args[i], template_params, bindings)) {
							return nullptr;
						}
					}
					for (const auto& param: function_template->params) {
						concrete_params.push_back(substitute_semantic_type(param, bindings));
					}
				}

				const std::string key = specialization_key(*function_template, concrete_params, bindings);
				if (const auto found = template_instances_.find(key); found != template_instances_.end()) {
					return found->second;
				}

				auto instance = std::make_unique<FunctionInfo>();
				instance->kind = function_template->kind;
				instance->owner = function_template->owner;
				instance->name = function_template->name;
				instance->owner_unit = function_template->owner_unit;
				instance->function = function_template->function;
				instance->method = function_template->method;
				instance->constructor = function_template->constructor;
				instance->destructor = function_template->destructor;
				instance->return_type = substitute_semantic_type(function_template->return_type, bindings);
				instance->params = concrete_params;
				instance->param_is_pack = function_template->param_is_pack;
				instance->template_params = function_template->template_params;
				instance->variadic = function_template->variadic;
				instance->is_extern = function_template->is_extern;
				instance->no_mangle = function_template->no_mangle;
				instance->external_only = function_template->external_only;
				instance->is_static_method = function_template->is_static_method;
				instance->is_template_instance = true;
				// For template struct methods, update the owner to the instantiated struct name
				// if the owner is a template struct
				if (template_structs_.contains(instance->owner)) {
					// The owner is a template struct - we need to find the instantiated version
					// The instantiated struct name should be in the form "BaseName<Args>"
					// We'll need to construct this from the bindings
					// For now, keep the original owner - the struct instantiation will handle this
				}
				if (instance->kind == FunctionKind::Method) {
					instance->llvm_name = mangle_method(*instance);
				} else if (instance->kind == FunctionKind::Constructor) {
					instance->llvm_name = mangle_constructor(*instance);
				} else if (instance->kind == FunctionKind::Destructor) {
					instance->llvm_name = mangle_destructor(*instance);
				} else {
					instance->llvm_name = mangle_free_function(*instance);
				}

				FunctionInfo* result = instance.get();
				owned_functions_.push_back(std::move(instance));
				// For template instances, the owner might be the base template name
				// Check if the owner exists in structs_, if not, check template_structs_
				const auto owner_it = structs_.find(result->owner);
				const auto template_it = template_structs_.find(result->owner);
				// If the owner is not found in either, this might be a template instance
				// where the owner should be updated to the instantiated struct name
				// We'll handle this by not adding to the owner's method list here
				// The struct instantiation code will handle this
				if (owner_it != structs_.end()) {
					if (result->kind == FunctionKind::Method) {
						owner_it->second.methods[result->name].push_back(result);
					} else if (result->kind == FunctionKind::Constructor) {
						owner_it->second.constructors.push_back(result);
					} else if (result->kind == FunctionKind::Destructor) {
						owner_it->second.destructor = result;
					} else {
						free_functions_[result->name].push_back(result);
					}
				} else if (template_it != template_structs_.end()) {
					if (result->kind == FunctionKind::Method) {
						template_it->second.methods[result->name].push_back(result);
					} else if (result->kind == FunctionKind::Constructor) {
						template_it->second.constructors.push_back(result);
					} else if (result->kind == FunctionKind::Destructor) {
						template_it->second.destructor = result;
					} else {
						free_functions_[result->name].push_back(result);
					}
				} else {
					// Owner not found - this is a template instance method
					// Don't add to any owner's list; it will be added by the struct instantiation
					free_functions_[result->name].push_back(result);
				}
				template_instances_[key] = result;

				std::vector<llvm::Type*> llvm_params;
				if ((result->kind == FunctionKind::Method && !result->is_static_method) ||
					result->kind == FunctionKind::Constructor ||
					result->kind == FunctionKind::Destructor) {
					llvm_params.push_back(llvm::PointerType::get(context_, 0));
				}
				for (const auto& parameter: result->params) {
					llvm_params.push_back(llvm_type(parameter, true));
				}
				llvm::FunctionType* function_type =
					llvm::FunctionType::get(llvm_type(result->return_type), llvm_params, result->variadic);
				result->llvm_function = llvm::Function::Create(function_type,
															   llvm::Function::ExternalLinkage,
															   result->llvm_name,
															   module_);
				return result;
			}

			llvm::Value* zero_value(const SemanticType& type) {
				if (type.is_void()) {
					return nullptr;
				}
				if (type.pointer_depth > 0 || type.is_reference || type.is_array) {
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

			llvm::Constant* global_string_constant(std::string_view escaped_literal) {
				std::string decoded(escaped_literal);
				if (decoded.size() >= 2 && decoded.front() == '"' && decoded.back() == '"') {
					decoded = decoded.substr(1, decoded.size() - 2);
				}
				decoded = unescape_literal(decoded);
				auto* data = llvm::ConstantDataArray::getString(context_, decoded, true);
				auto* global = new llvm::GlobalVariable(module_,
														data->getType(),
														true,
														llvm::GlobalValue::PrivateLinkage,
														data,
														std::format(".str.{}", string_id_++));
				global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
				global->setAlignment(llvm::MaybeAlign(1));
				llvm::Constant* indices[] = {
					llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0),
					llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0)};
				return llvm::ConstantExpr::getInBoundsGetElementPtr(data->getType(),
																	global,
																	llvm::ArrayRef<llvm::Constant*>(indices));
			}

			unsigned integer_bit_width(const SemanticType& type) const {
				if (type.name == "bool") {
					return 1;
				}
				if (type.name == "char" || type.name == "int8" || type.name == "uint8") {
					return 8;
				}
				if (type.name == "int16" || type.name == "uint16") {
					return 16;
				}
				if (type.name == "int32" || type.name == "uint32") {
					return 32;
				}
				if (type.name == "int64" || type.name == "uint64") {
					return 64;
				}
				return 32;
			}

			llvm::Constant* emit_constant_expression(const Expr* expr) {
				if (expr == nullptr) {
					return nullptr;
				}
				if (const auto* literal = dynamic_cast<const LiteralExpr*>(expr)) {
					if (literal->literal_kind == "String") {
						return global_string_constant(literal->value);
					}
					if (literal->literal_kind == "Character") {
						std::string decoded = literal->value;
						if (decoded.size() >= 2 && decoded.front() == '\'' && decoded.back() == '\'') {
							decoded = decoded.substr(1, decoded.size() - 2);
						}
						decoded = unescape_literal(decoded);
						const unsigned char value = decoded.empty() ? 0 : static_cast<unsigned char>(decoded.front());
						return llvm::ConstantInt::get(llvm::Type::getInt8Ty(context_), value);
					}
					if (literal->literal_kind == "KwTrue") {
						return llvm::ConstantInt::getTrue(context_);
					}
					if (literal->literal_kind == "KwFalse") {
						return llvm::ConstantInt::getFalse(context_);
					}
					if (literal->value.find('.') != std::string::npos) {
						return llvm::ConstantFP::get(llvm::Type::getDoubleTy(context_), std::stod(literal->value));
					}
					return llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), std::stoll(literal->value), true);
				}
				if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(expr)) {
					if (const auto found = globals_.find(identifier->name); found != globals_.end()) {
						if (found->second.global != nullptr && found->second.global->hasInitializer()) {
							return found->second.global->getInitializer();
						}
					}
					return nullptr;
				}
				return nullptr;
			}

			llvm::Constant* cast_constant(llvm::Constant* value, const SemanticType& from, const SemanticType& to) {
				if (value == nullptr || same_type(from, to)) {
					return value;
				}
				if (is_numeric_type(from) && is_numeric_type(to)) {
					if (is_float_type(from) && is_float_type(to)) {
						const auto* fp = llvm::dyn_cast<llvm::ConstantFP>(value);
						if (fp == nullptr) {
							return nullptr;
						}
						llvm::APFloat converted = fp->getValueAPF();
						bool loses_info = false;
						if (to.name == "float") {
							converted.convert(llvm::APFloat::IEEEsingle(), llvm::APFloat::rmNearestTiesToEven, &loses_info);
							return llvm::ConstantFP::get(context_, converted);
						}
						converted.convert(llvm::APFloat::IEEEdouble(), llvm::APFloat::rmNearestTiesToEven, &loses_info);
						return llvm::ConstantFP::get(context_, converted);
					}
					if (is_float_type(from) && is_integer_type(to)) {
						const auto* fp = llvm::dyn_cast<llvm::ConstantFP>(value);
						if (fp == nullptr) {
							return nullptr;
						}
						llvm::APSInt integer(integer_bit_width(to), !is_signed_integer_type(to));
						bool is_exact = false;
						if (fp->getValueAPF().convertToInteger(integer, llvm::APFloat::rmTowardZero, &is_exact) != llvm::APFloat::opOK) {
							return nullptr;
						}
						return llvm::ConstantInt::get(llvm_type(to), integer);
					}
					if (is_integer_type(from) && is_float_type(to)) {
						const auto* integer = llvm::dyn_cast<llvm::ConstantInt>(value);
						if (integer == nullptr) {
							return nullptr;
						}
						llvm::APFloat fp(to.name == "float" ? llvm::APFloat::IEEEsingle() : llvm::APFloat::IEEEdouble());
						if (is_signed_integer_type(from)) {
							fp.convertFromAPInt(integer->getValue(), true, llvm::APFloat::rmNearestTiesToEven);
						} else {
							fp.convertFromAPInt(integer->getValue(), false, llvm::APFloat::rmNearestTiesToEven);
						}
						return llvm::ConstantFP::get(context_, fp);
					}
					if (is_integer_type(from) && is_integer_type(to)) {
						const auto* integer = llvm::dyn_cast<llvm::ConstantInt>(value);
						if (integer == nullptr) {
							return nullptr;
						}
						llvm::APInt converted = integer->getValue();
						const unsigned to_bits = integer_bit_width(to);
						if (converted.getBitWidth() < to_bits) {
							converted = is_signed_integer_type(from) ? converted.sext(to_bits) : converted.zext(to_bits);
						} else if (converted.getBitWidth() > to_bits) {
							converted = converted.trunc(to_bits);
						}
						return llvm::ConstantInt::get(llvm_type(to), converted);
					}
				}
				if (from.is_array && to.pointer_depth > 0 && from.name == to.name) {
					return value;
				}
				return nullptr;
			}

			llvm::AllocaInst* create_entry_alloca(llvm::Function* function, llvm::Type* type, const std::string& name) {
				if (type == nullptr) {
					errors_.push_back(std::format("Cannot create alloca for null type (name: '{}')", name));
					return nullptr;
				}
				llvm::IRBuilder<> entry_builder(&function->getEntryBlock(), function->getEntryBlock().begin());
				return entry_builder.CreateAlloca(type, nullptr, name);
			}

			std::string static_local_key(const VarDeclStmt& variable) const {
				const std::string function_name = current_function_ != nullptr ? current_function_->getName().str() : "<global>";
				return std::format("{}:{}:{}:{}", function_name, variable.location.file, variable.location.line, variable.location.column);
			}

			StaticLocalInfo& ensure_static_local(const VarDeclStmt& variable, const SemanticType& type) {
				const std::string key = static_local_key(variable);
				if (const auto found = static_locals_.find(key); found != static_locals_.end()) {
					return found->second;
				}

				StaticLocalInfo info;
				info.type = type;
				llvm::Type* storage_type = llvm_type(type);
				llvm::Constant* initializer = nullptr;
				if (type.is_array) {
					initializer = llvm::ConstantAggregateZero::get(llvm::cast<llvm::ArrayType>(storage_type));
				} else {
					initializer = llvm::Constant::getNullValue(storage_type);
				}
				info.storage = new llvm::GlobalVariable(module_,
														storage_type,
														false,
														llvm::GlobalValue::InternalLinkage,
														initializer,
														std::format(".static.{}", static_local_id_++));
				info.guard = new llvm::GlobalVariable(module_,
													  llvm::Type::getInt1Ty(context_),
													  false,
													  llvm::GlobalValue::InternalLinkage,
													  llvm::ConstantInt::getFalse(context_),
													  std::format(".static.init.{}", static_local_id_++));
				auto [it, _] = static_locals_.emplace(key, std::move(info));
				register_static_local_cleanup(it->second);
				return it->second;
			}

			void register_static_local_cleanup(StaticLocalInfo& info) {
				if (info.cleanup != nullptr || !type_needs_static_cleanup(info.type)) {
					return;
				}

				SavedState state = save_state();

				llvm::FunctionType* cleanup_type = llvm::FunctionType::get(llvm::Type::getVoidTy(context_), false);
				llvm::Function* cleanup = llvm::Function::Create(cleanup_type,
																 llvm::GlobalValue::InternalLinkage,
																 std::format(".static.dtor.{}", static_local_id_++),
																 module_);
				info.cleanup = cleanup;

				llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", cleanup);
				llvm::BasicBlock* run_block = llvm::BasicBlock::Create(context_, "run", cleanup);
				llvm::BasicBlock* end_block = llvm::BasicBlock::Create(context_, "end", cleanup);

				builder_.SetInsertPoint(entry);
				current_unit_ = nullptr;
				current_struct_ = nullptr;
				current_self_ = nullptr;
				current_function_ = cleanup;
				current_yield_ = nullptr;
				scopes_.clear();
				current_template_bindings_.clear();
				current_pack_bindings_.clear();

				llvm::Value* initialized = builder_.CreateLoad(llvm::Type::getInt1Ty(context_), info.guard, "static.cleanup.init");
				builder_.CreateCondBr(initialized, run_block, end_block);

				builder_.SetInsertPoint(run_block);
				emit_static_cleanup_call(info.type, info.storage);
				if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
					builder_.CreateBr(end_block);
				}

				builder_.SetInsertPoint(end_block);
				if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
					builder_.CreateRetVoid();
				}

				restore_state(std::move(state));
			}

			void declare_structs() {
				for (auto& [name, info]: structs_) {
					if (info.llvm_type == nullptr) {
						info.llvm_type = llvm::StructType::create(context_, name);
					}
				}
				// Also declare template structs
				for (auto& [name, info]: template_structs_) {
					if (info.llvm_type == nullptr) {
						info.llvm_type = llvm::StructType::create(context_, name);
					}
				}
			}

			void declare_static_fields() {
				for (auto& [owner_name, info]: structs_) {
					for (auto& [field_name, field]: info.static_fields) {
						const std::string global_name = std::format("field {}.{}", owner_name, field_name);
						llvm::Constant* initializer = llvm::dyn_cast<llvm::Constant>(zero_value(field.type));
						if (field.decl != nullptr && field.decl->init != nullptr) {
							llvm::Constant* init_value = emit_constant_expression(field.decl->init.get());
							if (init_value == nullptr) {
								errors_.push_back(std::format("Static field '{}.{}' requires a constant initializer", owner_name, field_name));
							} else {
								init_value = cast_constant(init_value, infer_expr_type(field.decl->init.get()), field.type);
								if (init_value == nullptr) {
									errors_.push_back(std::format("Static field '{}.{}' requires a constant initializer", owner_name, field_name));
								} else {
									initializer = init_value;
								}
							}
						}
						field.global = new llvm::GlobalVariable(module_,
																llvm_type(field.type),
																field.type.is_const,
																llvm::GlobalValue::InternalLinkage,
																initializer,
																global_name);
					}
				}
			}

			void declare_globals() {
				for (auto& [name, global]: globals_) {
					global.global = new llvm::GlobalVariable(module_,
															 llvm_type(global.type),
															 global.type.is_const,
															 llvm::GlobalValue::ExternalLinkage,
															 nullptr,
															 name);
				}

				for (auto& [name, global]: globals_) {
					if (global.is_extern || global.global == nullptr) {
						continue;
					}

					llvm::Constant* initializer = nullptr;
					if (global.decl != nullptr) {
						if (!global.decl->array_init.empty()) {
							std::vector<llvm::Constant*> elements;
							elements.reserve(global.decl->array_init.size());
							SemanticType elem_type = element_type(global.type);
							bool all_constant = true;
							for (const auto& expr: global.decl->array_init) {
								llvm::Constant* value = emit_constant_expression(expr.get());
								if (value == nullptr) {
									errors_.push_back(std::format("Global array '{}' requires constant initializer expressions", name));
									all_constant = false;
									break;
								}
								value = cast_constant(value, infer_expr_type(expr.get()), elem_type);
								if (value == nullptr) {
									errors_.push_back(std::format("Global array '{}' requires constant initializer expressions", name));
									all_constant = false;
									break;
								}
								elements.push_back(value);
							}
							if (all_constant) {
								initializer = llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(llvm_type(global.type)), elements);
							}
						} else if (global.decl->init != nullptr) {
							initializer = emit_constant_expression(global.decl->init.get());
							if (initializer == nullptr) {
								errors_.push_back(std::format("Global '{}' requires a constant initializer", name));
							} else {
								initializer = cast_constant(initializer, infer_expr_type(global.decl->init.get()), global.type);
								if (initializer == nullptr) {
									errors_.push_back(std::format("Global '{}' requires a constant initializer", name));
								}
							}
						}
					}

					if (initializer == nullptr) {
						initializer = llvm::dyn_cast<llvm::Constant>(zero_value(global.type));
					}
					global.global->setLinkage(llvm::GlobalValue::InternalLinkage);
					global.global->setInitializer(initializer);
				}
			}

			void define_struct_layouts() {
				for (auto& [name, info]: structs_) {
					// Skip only template definitions (not instantiations)
					// Template definitions are stored in template_structs_, instantiations are in structs_
					if (template_structs_.contains(name)) {
						continue;
					}
					std::vector<llvm::Type*> field_types;
					field_types.reserve(info.fields.size());
					for (const auto& field: info.fields) {
						llvm::Type* field_type = llvm_type(field.type);
						if (field_type == nullptr) {
							errors_.push_back(std::format("Failed to get LLVM type for field '{}' in struct '{}'", field.name, name));
							return;
						}
						field_types.push_back(field_type);
					}
					if (!info.llvm_type->isOpaque()) {
						continue; // Already defined
					}
					info.llvm_type->setBody(field_types, false);
				}
			}

			void declare_functions() {
				for (auto& function: owned_functions_) {
					// Skip template definitions that are not instances
					// But keep variadic functions (they need to be declared for C linkage)
					// And keep external-only functions
					const bool is_member_of_template_definition =
						!function->owner.empty() && template_structs_.contains(function->owner) && !function->is_template_instance;
					if ((!function->template_params.empty() && !function->is_template_instance && !function->external_only && !function->variadic) ||
						is_member_of_template_definition) {
						continue;
					}
					std::vector<llvm::Type*> params;
					if ((function->kind == FunctionKind::Method && !function->is_static_method) || function->kind == FunctionKind::Conversion ||
						function->kind == FunctionKind::Destructor || function->kind == FunctionKind::Constructor) {
						params.push_back(llvm::PointerType::get(context_, 0));
					}
					for (const auto& parameter: function->params) {
						params.push_back(llvm_type(parameter, true));
					}
					llvm::FunctionType* function_type = llvm::FunctionType::get(llvm_type(function->return_type),
																				params,
																				function->variadic);
					function->llvm_function = llvm::Function::Create(function_type,
																	 llvm::Function::ExternalLinkage,
																	 function->llvm_name,
																	 module_);
					if (function->is_extern || function->no_mangle) {
						function->llvm_function->setCallingConv(llvm::CallingConv::C);
					}
				}
			}

			void define_functions() {
				for (auto& [name, info]: structs_) {
					for (FunctionInfo* constructor: info.constructors) {
						define_constructor(*constructor);
					}
					if (info.destructor != nullptr) {
						define_destructor(*info.destructor);
					}
					for (const auto& [method_name, methods]: info.methods) {
						for (FunctionInfo* method: methods) {
							define_method(*method);
						}
					}
					for (const auto& [conv_name, conv]: info.conversions) {
						define_conversion(*conv);
					}
				}
				for (const auto& [name, funcs]: free_functions_) {
					for (FunctionInfo* func: funcs) {
						define_free_function(*func);
					}
				}
			}

			void define_template_struct_functions() {
				// Define functions for template structs that were instantiated during code generation
				// These structs were added to structs_ after define_functions() ran
				// Collect template struct names first to avoid iterator invalidation
				std::vector<std::string> template_struct_names;
				for (const auto& [name, info]: structs_) {
					if (name.find('<') != std::string::npos) {
						template_struct_names.push_back(name);
					}
				}
				for (const std::string& name: template_struct_names) {
					auto it = structs_.find(name);
					if (it == structs_.end()) {
						continue;
					}
					StructInfo& info = it->second;
					for (FunctionInfo* constructor: info.constructors) {
						if (!constructor->is_defined && !constructor->is_defining) {
							define_constructor(*constructor);
						}
					}
					if (info.destructor != nullptr && !info.destructor->is_defined && !info.destructor->is_defining) {
						define_destructor(*info.destructor);
					}
					for (const auto& [method_name, methods]: info.methods) {
						for (FunctionInfo* method: methods) {
							if (!method->is_defined && !method->is_defining) {
								define_method(*method);
							}
						}
					}
				}
			}

			void define_template_function_instances() {
				// Define all template function instances that were created during code generation
				// These are functions that were instantiated via instantiate_template_function
				// but not yet defined
				// Use a while loop to handle cases where defining one function instantiates another
				bool changed = true;
				int iterations = 0;
				const int max_iterations = 100;
				while (changed && iterations < max_iterations) {
					changed = false;
					std::vector<FunctionInfo*> to_define;
					for (const auto& func: owned_functions_) {
						if (func->is_template_instance && !func->is_defined && !func->is_defining && !func->external_only) {
							to_define.push_back(func.get());
						}
					}
					for (FunctionInfo* func: to_define) {
						if (func->is_defined || func->is_defining) {
							continue;
						}
						changed = true;
						if (func->kind == FunctionKind::Method) {
							define_method(*func);
						} else if (func->kind == FunctionKind::Constructor) {
							define_constructor(*func);
						} else if (func->kind == FunctionKind::Destructor) {
							define_destructor(*func);
						}
					}
					iterations++;
				}
				if (iterations >= max_iterations) {
					errors_.push_back("Too many template function instantiation iterations - possible infinite loop");
				}
			}

			void finish_function(FunctionInfo& info) {
				if (builder_.GetInsertBlock() != nullptr && builder_.GetInsertBlock()->getTerminator() == nullptr) {
					if (info.kind == FunctionKind::Constructor) {
						// Constructors return void (they modify struct in-place via this pointer)
						builder_.CreateRetVoid();
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
				current_template_bindings_.clear();
				current_pack_bindings_.clear();
			}

			void bind_parameters(const std::vector<SemanticType>& parameters,
								 const std::vector<std::string>& names,
								 size_t start_index = 0) {
				for (size_t i = 0; i < names.size(); ++i) {
					llvm::Argument* arg = current_function_->getArg(static_cast<unsigned>(i + start_index));
					arg->setName(names[i]);
					llvm::Type* param_llvm_type = llvm_type(parameters[i], true);
					if (param_llvm_type == nullptr) {
						continue;
					}
					llvm::AllocaInst* slot = create_entry_alloca(current_function_, param_llvm_type, names[i]);
					if (slot == nullptr) {
						continue;
					}
					builder_.CreateStore(arg, slot);
					declare_variable(names[i], VariableInfo{parameters[i], slot, false});
				}
			}

			void bind_template_instance_parameters(const std::vector<Parameter>& parameters,
												   FunctionInfo& info,
												   size_t start_index = 0) {
				std::unordered_map<std::string, SemanticType> bindings;
				std::unordered_map<std::string, bool> template_param_names;
				for (const auto& param: info.template_params) {
					template_param_names[param.name] = param.is_pack;
				}
				size_t concrete_index = 0;
				for (const auto& parameter: parameters) {
					if (parameter.type.variadic) {
						continue;
					}
					if (parameter.is_pack) {
						while (concrete_index < info.params.size()) {
							if (template_param_names.contains(parameter.type.name)) {
								bindings[parameter.type.name] = info.params[concrete_index];
							}
							++concrete_index;
						}
						break;
					}
					if (template_param_names.contains(parameter.type.name)) {
						bindings[parameter.type.name] = info.params[concrete_index];
					}
					++concrete_index;
				}
				current_template_bindings_ = std::move(bindings);

				size_t arg_index = 0;
				for (const auto& parameter: parameters) {
					if (parameter.type.variadic) {
						continue;
					}
					if (parameter.is_pack) {
						std::vector<PackElementInfo> pack;
						while (arg_index < info.params.size()) {
							llvm::Argument* arg = current_function_->getArg(static_cast<unsigned>(arg_index + start_index));
							const std::string hidden_name = std::format("{}${}", parameter.name, pack.size());
							arg->setName(hidden_name);
							llvm::Type* param_type = llvm_type(info.params[arg_index], true);
							if (param_type == nullptr) {
								errors_.push_back(std::format("Failed to get LLVM type for parameter '{}' in template instance", hidden_name));
								++arg_index;
								continue;
							}
							llvm::AllocaInst* slot = create_entry_alloca(current_function_, param_type, hidden_name);
							if (slot == nullptr) {
								++arg_index;
								continue;
							}
							builder_.CreateStore(arg, slot);
							pack.push_back(PackElementInfo{info.params[arg_index], slot});
							++arg_index;
						}
						current_pack_bindings_[parameter.name] = std::move(pack);
						continue;
					}
					llvm::Argument* arg = current_function_->getArg(static_cast<unsigned>(arg_index + start_index));
					arg->setName(parameter.name);
					llvm::Type* param_type = llvm_type(info.params[arg_index], true);
					if (param_type == nullptr) {
						errors_.push_back(std::format("Failed to get LLVM type for parameter '{}' in template instance", parameter.name));
						++arg_index;
						continue;
					}
					llvm::AllocaInst* slot = create_entry_alloca(current_function_, param_type, parameter.name);
					if (slot == nullptr) {
						++arg_index;
						continue;
					}
					builder_.CreateStore(arg, slot);
					declare_variable(parameter.name, VariableInfo{info.params[arg_index], slot, false});
					++arg_index;
				}
			}

			void begin_function(FunctionInfo& info) {
				current_function_ = info.llvm_function;
				llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", current_function_);
				builder_.SetInsertPoint(entry);
			}

			void define_free_function(FunctionInfo& info) {
				// Skip external-only functions
				if (info.external_only) {
					return;
				}
				if (info.is_defined || info.is_defining) {
					return;
				}
				info.is_defining = true;
				begin_function(info);
				current_unit_ = info.owner_unit;
				if (info.is_template_instance && info.function != nullptr) {
					bind_template_instance_parameters(info.function->parameters, info);
				} else {
					std::vector<std::string> names;
					names.reserve(info.function->parameters.size());
					for (const auto& parameter: info.function->parameters) {
						names.push_back(parameter.name);
					}
					bind_parameters(info.params, names);
				}
				emit_statement(info.function->body.get());
				finish_function(info);
				info.is_defining = false;
				info.is_defined = true;
			}

			void define_constructor(FunctionInfo& info) {
				// Skip external-only functions
				if (info.external_only) {
						return;
				}
				if (info.is_defined || info.is_defining) {
						return;
				}
				info.is_defining = true;
				const auto it = structs_.find(info.owner);
				const auto template_it = template_structs_.find(info.owner);
				if (it == structs_.end() && template_it == template_structs_.end()) {
					errors_.push_back(std::format("Cannot find owner struct '{}' for constructor", info.owner));
					return;
				}
				const StructInfo& owner = (it != structs_.end()) ? it->second : template_it->second;
				begin_function(info);
				current_unit_ = info.owner_unit;
				// Use the first argument as the this pointer (struct to initialize in-place)
				llvm::Argument* this_arg = current_function_->getArg(0);
				this_arg->setName("this");
				current_self_ = this_arg;
				current_struct_ = &owner;
				declare_variable("this", VariableInfo{SemanticType{info.owner, false, false, true}, this_arg, false});

				// For template instances, we need to set up template bindings
				if (info.is_template_instance) {
					// Parse the instantiated type name to extract template arguments
					size_t lt_pos = info.owner.find('<');
					size_t gt_pos = info.owner.rfind('>');
					if (lt_pos != std::string::npos && gt_pos != std::string::npos && gt_pos > lt_pos) {
						std::string base_name = info.owner.substr(0, lt_pos);
						std::string args_str = info.owner.substr(lt_pos + 1, gt_pos - lt_pos - 1);
						// Find the base template struct
						const auto base_it = template_structs_.find(base_name);
						if (base_it != template_structs_.end() && base_it->second.decl != nullptr) {
							// Set up template bindings
							std::unordered_map<std::string, SemanticType> bindings;
							// Add binding for base template name to instantiated name
							bindings[base_name] = SemanticType{info.owner, false, false, false};
							const auto& template_params = base_it->second.decl->template_params;
							// Parse arguments (simple comma-separated for now)
							size_t start = 0;
							size_t comma_pos = 0;
							size_t param_idx = 0;
							while (comma_pos != std::string::npos && param_idx < template_params.size()) {
								comma_pos = args_str.find(',', start);
								std::string arg_str = (comma_pos == std::string::npos) 
									? args_str.substr(start) 
									: args_str.substr(start, comma_pos - start);
								// Trim whitespace
								size_t first_non_space = arg_str.find_first_not_of(" \t");
								size_t last_non_space = arg_str.find_last_not_of(" \t");
								if (first_non_space != std::string::npos) {
									arg_str = arg_str.substr(first_non_space, last_non_space - first_non_space + 1);
								}
								// Create SemanticType from the argument string
								SemanticType arg_type = from_typeref_string(arg_str);
								bindings[template_params[param_idx].name] = arg_type;
								start = comma_pos + 1;
								param_idx++;
							}
							current_template_bindings_ = bindings;
						}
					}
				}

				if (info.is_template_instance && !info.template_params.empty() && info.constructor != nullptr) {
						bind_template_instance_parameters(info.constructor->parameters, info, 1);
				} else {
						std::vector<std::string> names;
					names.reserve(info.constructor->parameters.size());
					for (const auto& parameter: info.constructor->parameters) {
						names.push_back(parameter.name);
					}
					// Skip first argument (this pointer) when binding parameters
					bind_parameters(info.params, names, 1);
				}
				emit_statement(info.constructor->body.get());
				current_template_bindings_.clear();
				finish_function(info);
				info.is_defining = false;
				info.is_defined = true;
			}

			void define_destructor(FunctionInfo& info) {
				// Skip external-only functions
				if (info.external_only) {
					return;
				}
				if (info.is_defined || info.is_defining) {
					return;
				}
				info.is_defining = true;
				const auto it = structs_.find(info.owner);
				const auto template_it = template_structs_.find(info.owner);
				if (it == structs_.end() && template_it == template_structs_.end()) {
					errors_.push_back(std::format("Cannot find owner struct '{}' for destructor", info.owner));
					return;
				}
				const StructInfo& owner = (it != structs_.end()) ? it->second : template_it->second;
				begin_function(info);
				current_unit_ = info.owner_unit;
				llvm::Argument* this_arg = current_function_->getArg(0);
				this_arg->setName("this");
				current_self_ = this_arg;
				current_struct_ = &owner;
				declare_variable("this", VariableInfo{SemanticType{info.owner, false, false, true}, this_arg, false});

				// For template instances, we need to set up template bindings
				if (info.is_template_instance) {
					// Parse the instantiated type name to extract template arguments
					size_t lt_pos = info.owner.find('<');
					size_t gt_pos = info.owner.rfind('>');
					if (lt_pos != std::string::npos && gt_pos != std::string::npos && gt_pos > lt_pos) {
						std::string base_name = info.owner.substr(0, lt_pos);
						std::string args_str = info.owner.substr(lt_pos + 1, gt_pos - lt_pos - 1);
						// Find the base template struct
						const auto base_it = template_structs_.find(base_name);
						if (base_it != template_structs_.end() && base_it->second.decl != nullptr) {
							// Set up template bindings
							std::unordered_map<std::string, SemanticType> bindings;
							// Add binding for base template name to instantiated name
							bindings[base_name] = SemanticType{info.owner, false, false, false};
							const auto& template_params = base_it->second.decl->template_params;
							// Parse arguments (simple comma-separated for now)
							size_t start = 0;
							size_t comma_pos = 0;
							size_t param_idx = 0;
							while (comma_pos != std::string::npos && param_idx < template_params.size()) {
								comma_pos = args_str.find(',', start);
								std::string arg_str = (comma_pos == std::string::npos) 
									? args_str.substr(start) 
									: args_str.substr(start, comma_pos - start);
								// Trim whitespace
								size_t first_non_space = arg_str.find_first_not_of(" \t");
								size_t last_non_space = arg_str.find_last_not_of(" \t");
								if (first_non_space != std::string::npos) {
									arg_str = arg_str.substr(first_non_space, last_non_space - first_non_space + 1);
								}
								// Create SemanticType from the argument string
								SemanticType arg_type = from_typeref_string(arg_str);
								bindings[template_params[param_idx].name] = arg_type;
								start = comma_pos + 1;
								param_idx++;
							}
							current_template_bindings_ = bindings;
						}
					}
				}

				emit_statement(info.destructor->body.get());
				current_template_bindings_.clear();
				finish_function(info);
				info.is_defining = false;
				info.is_defined = true;
			}

			void define_method(FunctionInfo& info) {
				// Skip template definitions that are not instances
				// Also skip external-only functions (they're just declarations)
				if (!info.template_params.empty() && !info.is_template_instance && !info.external_only) {
					return;
				}
				// Skip external-only functions (they're just declarations for C linkage)
				if (info.external_only) {
					return;
				}
				if (info.is_defined || info.is_defining) {
					return;
				}
				info.is_defining = true;
				const auto it = structs_.find(info.owner);
				const auto template_it = template_structs_.find(info.owner);
				if (it == structs_.end() && template_it == template_structs_.end()) {
					errors_.push_back(std::format("Cannot find owner struct '{}' for method", info.owner));
					return;
				}
				const StructInfo& owner = (it != structs_.end()) ? it->second : template_it->second;
				begin_function(info);
				current_unit_ = info.owner_unit;
				current_struct_ = &owner;
				if (!info.is_static_method) {
					llvm::Argument* this_arg = current_function_->getArg(0);
					this_arg->setName("this");
					current_self_ = this_arg;
					declare_variable("this", VariableInfo{SemanticType{info.owner, false, false, true}, this_arg, false});
				}

				// For template instances, we need to set up template bindings
				if (info.is_template_instance) {
					// Parse the instantiated type name to extract template arguments
					size_t lt_pos = info.owner.find('<');
					size_t gt_pos = info.owner.rfind('>');
					if (lt_pos != std::string::npos && gt_pos != std::string::npos && gt_pos > lt_pos) {
						std::string base_name = info.owner.substr(0, lt_pos);
						std::string args_str = info.owner.substr(lt_pos + 1, gt_pos - lt_pos - 1);
						// Find the base template struct
						const auto base_it = template_structs_.find(base_name);
						if (base_it != template_structs_.end() && base_it->second.decl != nullptr) {
							// Set up template bindings
							std::unordered_map<std::string, SemanticType> bindings;
							// Add binding for base template name to instantiated name
							bindings[base_name] = SemanticType{info.owner, false, false, false};
							const auto& template_params = base_it->second.decl->template_params;
							// Parse arguments (simple comma-separated for now)
							size_t start = 0;
							size_t comma_pos = 0;
							size_t param_idx = 0;
							while (comma_pos != std::string::npos && param_idx < template_params.size()) {
								comma_pos = args_str.find(',', start);
								std::string arg_str = (comma_pos == std::string::npos) 
									? args_str.substr(start) 
									: args_str.substr(start, comma_pos - start);
								// Trim whitespace
								size_t first_non_space = arg_str.find_first_not_of(" \t");
								size_t last_non_space = arg_str.find_last_not_of(" \t");
								if (first_non_space != std::string::npos) {
									arg_str = arg_str.substr(first_non_space, last_non_space - first_non_space + 1);
								}
								// Create SemanticType from the argument string
								SemanticType arg_type = from_typeref_string(arg_str);
								bindings[template_params[param_idx].name] = arg_type;
								start = comma_pos + 1;
								param_idx++;
							}
							current_template_bindings_ = bindings;
						}
					}
				}

				if (info.is_template_instance && info.method != nullptr) {
					bind_template_instance_parameters(info.method->parameters, info, info.is_static_method ? 0 : 1);
				} else {
					std::vector<std::string> names;
					names.reserve(info.method->parameters.size());
					for (const auto& parameter: info.method->parameters) {
						names.push_back(parameter.name);
					}
					bind_parameters(info.params, names, info.is_static_method ? 0 : 1);
				}
				emit_statement(info.method->body.get());
				current_template_bindings_.clear();
				finish_function(info);
				info.is_defining = false;
				info.is_defined = true;
			}

			void define_conversion(FunctionInfo& info) {
				// Skip external-only functions
				if (info.external_only) {
					return;
				}
				const auto it = structs_.find(info.owner);
				const auto template_it = template_structs_.find(info.owner);
				if (it == structs_.end() && template_it == template_structs_.end()) {
					errors_.push_back(std::format("Cannot find owner struct '{}' for conversion", info.owner));
					return;
				}
				const StructInfo& owner = (it != structs_.end()) ? it->second : template_it->second;
				begin_function(info);
				current_unit_ = info.owner_unit;
				llvm::Argument* this_arg = current_function_->getArg(0);
				this_arg->setName("this");
				current_self_ = this_arg;
				current_struct_ = &owner;
				declare_variable("this", VariableInfo{SemanticType{info.owner, false, false, true}, this_arg, false});
				emit_statement(info.conversion->body.get());
				finish_function(info);
			}

			std::unordered_map<std::string, std::string> current_modules() const {
				std::unordered_map<std::string, std::string> modules;
				if (current_unit_ == nullptr) {
					return modules;
				}
				for (const auto& decl: current_unit_->declarations) {
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

			std::optional<std::string> referenced_struct_name(const Expr* expr) const {
				const auto* identifier = dynamic_cast<const IdentifierExpr*>(expr);
				if (identifier == nullptr || !structs_.contains(identifier->name) || lookup_variable(identifier->name) != nullptr ||
					globals_.contains(identifier->name)) {
					return std::nullopt;
				}
				return identifier->name;
			}

			const FieldInfo* lookup_field(const StructInfo& owner, const std::string& name) const {
				if (const auto found = owner.field_indices.find(name); found != owner.field_indices.end()) {
					return &owner.fields[found->second];
				}
				return nullptr;
			}

			SemanticType infer_expr_type(const Expr* expr) {
				if (expr == nullptr) {
					return SemanticType{"void"};
				}
				if (const auto* literal = dynamic_cast<const LiteralExpr*>(expr)) {
					if (literal->literal_kind == "String") {
						SemanticType type;
						type.name = "char";
						type.pointer_depth = 1;
						type.is_const = true;
						return type;
					}
					if (literal->literal_kind == "Character") {
						return SemanticType{"char"};
					}
					if (literal->literal_kind == "KwTrue" || literal->literal_kind == "KwFalse") {
						return SemanticType{"bool"};
					}
					if (literal->value.find('.') != std::string::npos) {
						return SemanticType{"double"};
					}
					return SemanticType{"int32"};
				}
				if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(expr)) {
					if (identifier->name == "this" && current_struct_ != nullptr && current_self_ != nullptr) {
						// Use the actual struct name from structs_ if this is a template instance
						std::string struct_name = current_struct_->decl->name;
						// Check if this is a template instance by looking for the struct in structs_
						for (const auto& [name, info] : structs_) {
							if (&info == current_struct_) {
								struct_name = name;
								break;
							}
						}
						SemanticType self_type{struct_name};
						self_type.pointer_depth = 1;
						return self_type;
					}
					if (const VariableInfo* variable = lookup_variable(identifier->name)) {
						return variable->type;
					}
					if (const auto global = globals_.find(identifier->name); global != globals_.end()) {
						return global->second.type;
					}
					if (current_struct_ != nullptr) {
						if (const auto static_field = current_struct_->static_fields.find(identifier->name);
							static_field != current_struct_->static_fields.end()) {
							return static_field->second.type;
						}
					}
					if (current_struct_ != nullptr && current_self_ != nullptr) {
						if (const FieldInfo* field = lookup_field(*current_struct_, identifier->name)) {
							return field->type;
						}
					}
					if (structs_.contains(identifier->name)) {
						return SemanticType{"<type>"};
					}
					if (free_functions_.contains(identifier->name)) {
						return SemanticType{"<function>"};
					}
					if (is_module_identifier(expr)) {
						return SemanticType{"<module>"};
					}
					return SemanticType{identifier->name, false, false, false, false, false, 0, true};
				}
				if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
					SemanticType operand = infer_expr_type(unary->operand.get());
					if (unary->op == "*" && (operand.pointer_depth > 0 || operand.is_array || operand.is_reference)) {
						return element_type(operand);
					}
					if (unary->op == "&") {
						operand.pointer_depth++;
						operand.is_array = false;
						return operand;
					}
					if (unary->op == "!") {
						return SemanticType{"bool"};
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
						return SemanticType{"bool"};
					}
					if (is_numeric_type(lhs) && is_numeric_type(rhs)) {
						return numeric_common_type(lhs, rhs);
					}
					return lhs;
				}
				if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
					std::string module_path;
					if (is_module_identifier(member->object.get(), &module_path)) {
						if (const auto global = globals_.find(member->member); global != globals_.end()) {
							return global->second.type;
						}
					}
					if (auto static_owner = referenced_struct_name(member->object.get())) {
						const auto found = structs_.find(*static_owner);
						if (found != structs_.end()) {
							if (const auto static_field = found->second.static_fields.find(member->member);
								static_field != found->second.static_fields.end()) {
								return static_field->second.type;
							}
							if (const auto methods = found->second.methods.find(member->member); methods != found->second.methods.end()) {
								for (FunctionInfo* function: methods->second) {
									if (function != nullptr && function->is_static_method) {
										return function->return_type;
									}
								}
							}
						}
						return SemanticType{"<error>", false, false, false, false, false, 0, true};
					}
					SemanticType object_type = infer_expr_type(member->object.get());
					// Always zero pointer_depth before looking up struct - MemberExpr accesses the struct itself
					object_type.pointer_depth = 0;
					object_type.is_reference = false;
					object_type.is_array = false;
					object_type.array_size = 0;
						if (const auto found = structs_.find(object_type.name); found != structs_.end()) {
						if (const FieldInfo* field = lookup_field(found->second, member->member)) {
							// Substitute template parameters in field type
							return substitute_semantic_type(field->type, current_template_bindings_);
						}
						if (const auto methods = found->second.methods.find(member->member); methods != found->second.methods.end() &&
																							 !methods->second.empty()) {
							return methods->second.front()->return_type;
						}
					}
					return SemanticType{"<error>", false, false, false, false, false, 0, true};
				}
				if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
					return element_type(infer_expr_type(index->object.get()));
				}
				if (const auto* cast = dynamic_cast<const TypeCastExpr*>(expr)) {
					return from_typeref(cast->target_type);
				}
				if (const auto* alloc = dynamic_cast<const NewExpr*>(expr)) {
					// Use substitute_typeref to handle template parameter substitution
					SemanticType type = substitute_typeref(alloc->target_type);
					type.pointer_depth = 1;
					return type;
				}
				if (dynamic_cast<const DestructorCallExpr*>(expr)) {
					return SemanticType{"void"};
				}
				if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
					CallResolution resolution = resolve_call(call);
					if (resolution.function != nullptr) {
						// Constructor expressions evaluate to constructed values.
						if (resolution.function->kind == FunctionKind::Constructor) {
							return SemanticType{resolution.function->owner};
						}
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
					for (const auto& match_case: match_expr->cases) {
						if (match_case.fallthrough) {
							continue;
						}
						return infer_branch_type(match_case.body);
					}
				}
				return SemanticType{"<error>", false, false, false, false, false, 0, true};
			}

			SemanticType infer_branch_type(const std::variant<ExprPtr, std::unique_ptr<BlockStmt>>& branch) {
				if (const auto* expr = std::get_if<ExprPtr>(&branch)) {
					return infer_expr_type(expr->get());
				}
				return infer_block_yield_type(std::get<std::unique_ptr<BlockStmt>>(branch).get());
			}

			SemanticType infer_block_yield_type(const BlockStmt* block) {
				if (block == nullptr) {
					return SemanticType{"void"};
				}
				for (const auto& statement: block->statements) {
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
				return SemanticType{"void"};
			}

			llvm::Value* emit_literal(const LiteralExpr& literal) {
				if (literal.literal_kind == "String") {
					std::string decoded = literal.value;
					if (decoded.size() >= 2 && decoded.front() == '"' && decoded.back() == '"') {
						decoded = decoded.substr(1, decoded.size() - 2);
					}
					decoded = unescape_literal(decoded);
					llvm::GlobalVariable* global = builder_.CreateGlobalString(decoded, std::format(".str.{}", string_id_++));
					return builder_.CreateConstInBoundsGEP2_32(global->getValueType(), global, 0, 0, "str.ptr");
				}
				if (literal.literal_kind == "Character") {
					std::string decoded = literal.value;
					if (decoded.size() >= 2 && decoded.front() == '\'' && decoded.back() == '\'') {
						decoded = decoded.substr(1, decoded.size() - 2);
					}
					decoded = unescape_literal(decoded);
					const unsigned char value = decoded.empty() ? 0 : static_cast<unsigned char>(decoded.front());
					return llvm::ConstantInt::get(llvm::Type::getInt8Ty(context_), value);
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
				llvm::Type* ty = llvm_type(variable.type);
				return builder_.CreateLoad(ty, variable.address, name);
			}

			llvm::Value* emit_lvalue(const Expr* expr) {
				if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(expr)) {
					if (identifier->name == "this" && current_self_ != nullptr) {
						return current_self_;
					}
					if (VariableInfo* variable = lookup_variable(identifier->name)) {
						return variable->address;
					}
					if (const auto global = globals_.find(identifier->name); global != globals_.end()) {
						return global->second.global;
					}
					if (current_struct_ != nullptr) {
						if (const auto static_field = current_struct_->static_fields.find(identifier->name);
							static_field != current_struct_->static_fields.end()) {
							return static_field->second.global;
						}
					}
					if (current_struct_ != nullptr && current_self_ != nullptr) {
						if (const FieldInfo* field = lookup_field(*current_struct_, identifier->name)) {
							return builder_.CreateStructGEP(current_struct_->llvm_type, current_self_, field->index, identifier->name + ".addr");
						}
					}
				}
				if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
					std::string module_path;
					if (is_module_identifier(member->object.get(), &module_path)) {
						if (const auto global = globals_.find(member->member); global != globals_.end()) {
							return global->second.global;
						}
					}
					if (auto static_owner = referenced_struct_name(member->object.get())) {
						const auto found = structs_.find(*static_owner);
						if (found != structs_.end()) {
							if (const auto static_field = found->second.static_fields.find(member->member);
								static_field != found->second.static_fields.end()) {
								return static_field->second.global;
							}
						}
						errors_.push_back(std::format("Static member '{}.{}' is not an lvalue", *static_owner, member->member));
						return nullptr;
					}
					SemanticType object_type = infer_expr_type(member->object.get());
					llvm::Value* base_address = nullptr;
					if (member->via_arrow) {
						base_address = emit_expression(member->object.get());
					} else {
						base_address = emit_lvalue(member->object.get());
					}
					if (base_address == nullptr) {
						errors_.push_back(std::format("Cannot take address of member '{}'", member->member));
						return nullptr;
					}
					// Always zero pointer_depth before looking up struct - MemberExpr accesses the struct itself
					object_type.pointer_depth = 0;
					object_type.is_reference = false;
					object_type.is_array = false;
					object_type.array_size = 0;
					// Check both structs_ and template_structs_ for the struct type
					const auto found = structs_.find(object_type.name);
					const auto template_found = template_structs_.find(object_type.name);
					if (found == structs_.end() && template_found == template_structs_.end()) {
						errors_.push_back(std::format("Unknown struct type '{}'", object_type.name));
						return nullptr;
					}
					const StructInfo& struct_info = (found != structs_.end()) ? found->second : template_found->second;
					const FieldInfo* field = lookup_field(struct_info, member->member);
					if (field == nullptr) {
						errors_.push_back(std::format("Struct '{}' has no field '{}'", object_type.name, member->member));
						return nullptr;
					}
					return builder_.CreateStructGEP(struct_info.llvm_type, base_address, field->index, member->member + ".addr");
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
						if (const auto global = globals_.find(identifier->name); global != globals_.end()) {
						return load_variable(VariableInfo{global->second.type, global->second.global, false}, identifier->name);
					}
					if (current_struct_ != nullptr) {
						if (const auto static_field = current_struct_->static_fields.find(identifier->name);
							static_field != current_struct_->static_fields.end()) {
							return builder_.CreateLoad(llvm_type(static_field->second.type), static_field->second.global, identifier->name);
						}
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
					std::string module_path;
					if (is_module_identifier(member->object.get(), &module_path)) {
						if (const auto global = globals_.find(member->member); global != globals_.end()) {
							return load_variable(VariableInfo{global->second.type, global->second.global, false}, member->member);
						}
					}
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
				if (const auto* destructor = dynamic_cast<const DestructorCallExpr*>(expr)) {
					return emit_destructor_expression(*destructor);
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
				
				// Handle pointer arithmetic: pointer + integer
				if (binary.op == "+" || binary.op == "-") {
					if (lhs_type.pointer_depth > 0 && is_numeric_type(rhs_type)) {
						llvm::Value* ptr = emit_expression(binary.lhs.get());
						llvm::Value* offset = emit_expression(binary.rhs.get());
						if (ptr == nullptr || offset == nullptr) {
							return nullptr;
						}
						// Cast offset to i64 for GEP
						if (!offset->getType()->isIntegerTy(64)) {
							offset = builder_.CreateIntCast(offset, llvm::Type::getInt64Ty(context_), false, "offset.cast");
						}
						// Get element type of the pointer
						SemanticType element_type = lhs_type;
						element_type.pointer_depth--;
						llvm::Type* llvm_element_type = llvm_type(element_type);
						// Use GEP for pointer arithmetic
						if (binary.op == "+") {
							return builder_.CreateInBoundsGEP(llvm_element_type, ptr, offset, "ptr.add");
						} else {
							// Subtract: ptr - offset = ptr + (-offset)
							llvm::Value* neg_offset = builder_.CreateNeg(offset, "neg.offset");
							return builder_.CreateInBoundsGEP(llvm_element_type, ptr, neg_offset, "ptr.sub");
						}
					}
					if (rhs_type.pointer_depth > 0 && is_numeric_type(lhs_type) && binary.op == "+") {
						// integer + pointer (commutative)
						llvm::Value* ptr = emit_expression(binary.rhs.get());
						llvm::Value* offset = emit_expression(binary.lhs.get());
						if (ptr == nullptr || offset == nullptr) {
							return nullptr;
						}
						if (!offset->getType()->isIntegerTy(64)) {
							offset = builder_.CreateIntCast(offset, llvm::Type::getInt64Ty(context_), false, "offset.cast");
						}
						SemanticType element_type = rhs_type;
						element_type.pointer_depth--;
						llvm::Type* llvm_element_type = llvm_type(element_type);
						return builder_.CreateInBoundsGEP(llvm_element_type, ptr, offset, "ptr.add");
					}
				}
				
				if (is_numeric_type(lhs_type) && is_numeric_type(rhs_type)) {
					SemanticType common = numeric_common_type(lhs_type, rhs_type);
					llvm::Value* lhs = cast_value(emit_expression(binary.lhs.get()), lhs_type, common, false);
					llvm::Value* rhs = cast_value(emit_expression(binary.rhs.get()), rhs_type, common, false);
					if (lhs == nullptr || rhs == nullptr) {
						return nullptr;
					}
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
				if (lhs_type.is_const) {
					errors_.push_back(std::format("Cannot assign to const object of type '{}'", type_to_string(lhs_type)));
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
				YieldContext yield_context{slot, merge_block, result_type, scopes_.size()};
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

			llvm::Value* emit_destructor_expression(const DestructorCallExpr& expr) {
				SemanticType object_type = infer_expr_type(expr.object.get());
				SemanticType base_type = object_type;
				llvm::Value* address = nullptr;

				if (expr.via_arrow) {
					address = emit_expression(expr.object.get());
					base_type.pointer_depth = 0;
					base_type.is_reference = false;
				} else {
					address = ensure_address(expr.object.get(), object_type);
					base_type.is_reference = false;
				}

				if (address == nullptr) {
					errors_.push_back(std::format("Unable to emit destructor call for type '{}'", expr.type_name));
					return nullptr;
				}

				emit_destructor_call(base_type, address);
				return nullptr;
			}

			llvm::Value* emit_new_expression(const NewExpr& expr) {
				// Use substitute_typeref to handle template parameter substitution
				SemanticType allocated_type = substitute_typeref(expr.target_type);
				llvm::Value* element_count = expr.is_array ? emit_expression(expr.array_size.get())
														   : llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 1);
				if (element_count == nullptr) {
					return nullptr;
				}
				if (!element_count->getType()->isIntegerTy(64)) {
					element_count = builder_.CreateIntCast(element_count, llvm::Type::getInt64Ty(context_), false, "new.count.cast");
				}
				llvm::Value* storage =
					expr.placement ? emit_expression(expr.placement.get()) : allocate_heap_block(allocated_type, element_count);
				if (storage == nullptr) {
					return nullptr;
				}

				if (const auto struct_it = structs_.find(allocated_type.name); struct_it != structs_.end()) {
					std::vector<SemanticType> arg_types;
					arg_types.reserve(expr.args.size());
					for (const auto& arg: expr.args) {
						arg_types.push_back(infer_expr_type(arg.get()));
					}
					FunctionInfo* ctor = choose_overload(struct_it->second.constructors, arg_types);
					if (ctor == nullptr) {
						errors_.push_back(std::format("Unable to resolve constructor for new {}", allocated_type.name));
						return storage;
					}

					std::vector<llvm::Value*> ctor_args;
					ctor_args.reserve(expr.args.size());
					for (size_t i = 0; i < expr.args.size(); ++i) {
						llvm::Value* value = emit_expression(expr.args[i].get());
						value = cast_value(value, arg_types[i], ctor->params[i], true);
						ctor_args.push_back(value);
					}

					if (!expr.is_array) {
						llvm::Value* constructed = builder_.CreateCall(ctor->llvm_function, ctor_args, "new.value");
						builder_.CreateStore(constructed, storage);
						return storage;
					}

					llvm::Function* function = current_function_;
					llvm::BasicBlock* cond_block = llvm::BasicBlock::Create(context_, "new.array.struct.cond", function);
					llvm::BasicBlock* body_block = llvm::BasicBlock::Create(context_, "new.array.struct.body", function);
					llvm::BasicBlock* end_block = llvm::BasicBlock::Create(context_, "new.array.struct.end", function);
					llvm::AllocaInst* index_slot = create_entry_alloca(current_function_, llvm::Type::getInt64Ty(context_), "new.array.index");
					builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0), index_slot);
					builder_.CreateBr(cond_block);

					builder_.SetInsertPoint(cond_block);
					llvm::Value* index = builder_.CreateLoad(llvm::Type::getInt64Ty(context_), index_slot, "new.array.index");
					llvm::Value* condition = builder_.CreateICmpULT(index, element_count, "new.array.more");
					builder_.CreateCondBr(condition, body_block, end_block);

					builder_.SetInsertPoint(body_block);
					llvm::Value* element_address = builder_.CreateInBoundsGEP(llvm_type(allocated_type), storage, index, "new.array.elem.addr");
					llvm::Value* constructed = builder_.CreateCall(ctor->llvm_function, ctor_args, "new.array.value");
					builder_.CreateStore(constructed, element_address);
					llvm::Value* next = builder_.CreateAdd(index, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1), "new.array.next");
					builder_.CreateStore(next, index_slot);
					builder_.CreateBr(cond_block);

					builder_.SetInsertPoint(end_block);
					return storage;
				}

				if (!expr.is_array && expr.args.size() == 1) {
					llvm::Value* init = emit_expression(expr.args[0].get());
					SemanticType init_type = infer_expr_type(expr.args[0].get());
					builder_.CreateStore(cast_value(init, init_type, allocated_type, true), storage);
					return storage;
				}

				llvm::Value* init_value = nullptr;
				if (expr.args.size() == 1) {
					init_value = emit_expression(expr.args[0].get());
					SemanticType init_type = infer_expr_type(expr.args[0].get());
					init_value = cast_value(init_value, init_type, allocated_type, true);
				} else {
					init_value = zero_value(allocated_type);
				}

				if (!expr.is_array) {
					builder_.CreateStore(init_value, storage);
				} else {
					llvm::Function* function = current_function_;
					llvm::BasicBlock* cond_block = llvm::BasicBlock::Create(context_, "new.array.cond", function);
					llvm::BasicBlock* body_block = llvm::BasicBlock::Create(context_, "new.array.body", function);
					llvm::BasicBlock* end_block = llvm::BasicBlock::Create(context_, "new.array.end", function);
					llvm::AllocaInst* index_slot = create_entry_alloca(current_function_, llvm::Type::getInt64Ty(context_), "new.array.index");
					builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0), index_slot);
					builder_.CreateBr(cond_block);

					builder_.SetInsertPoint(cond_block);
					llvm::Value* index = builder_.CreateLoad(llvm::Type::getInt64Ty(context_), index_slot, "new.array.index");
					llvm::Value* condition = builder_.CreateICmpULT(index, element_count, "new.array.more");
					builder_.CreateCondBr(condition, body_block, end_block);

					builder_.SetInsertPoint(body_block);
					llvm::Value* element_address = builder_.CreateInBoundsGEP(llvm_type(allocated_type), storage, index, "new.array.elem.addr");
					builder_.CreateStore(init_value, element_address);
					llvm::Value* next = builder_.CreateAdd(index, llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1), "new.array.next");
					builder_.CreateStore(next, index_slot);
					builder_.CreateBr(cond_block);

					builder_.SetInsertPoint(end_block);
				}
				if (!expr.is_array && expr.args.empty()) {
					builder_.CreateStore(zero_value(allocated_type), storage);
				}
				return storage;
			}

			llvm::Value* cast_value(llvm::Value* value, const SemanticType& from, const SemanticType& to, bool implicit) {
				if (value == nullptr || same_type(from, to)) {
					return value;
				}
				// Handle casting from base template type to instantiated template type
				// e.g., Array -> Array<int32> (this should be a no-op if they're the same underlying type)
				if (!from.name.empty() && !to.name.empty()) {
					// Check if from is a base template and to is an instantiation of it
					size_t to_lt = to.name.find('<');
					if (to_lt != std::string::npos) {
						std::string to_base = to.name.substr(0, to_lt);
						if (from.name == to_base) {
							// This is a cast from base template to its instantiation
							// They should have the same LLVM type, so just return the value
							return value;
						}
					}
					// Check if to is a base template and from is an instantiation of it
					size_t from_lt = from.name.find('<');
					if (from_lt != std::string::npos) {
						std::string from_base = from.name.substr(0, from_lt);
						if (to.name == from_base) {
							// This is a cast from instantiation to base template
							// They should have the same LLVM type, so just return the value
							return value;
						}
					}
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
				if (!implicit && !from.pointer_depth > 0 && !from.is_reference && !from.is_array) {
					if (const auto struct_it = structs_.find(from.name); struct_it != structs_.end()) {
						if (const auto conversion = struct_it->second.conversions.find(to.name); conversion != struct_it->second.conversions.end()) {
							llvm::Value* address = materialize_address(value, from, "cast.tmp");
							return builder_.CreateCall(conversion->second->llvm_function, {address}, "cast.call");
						}
					}
				}
				if (from.is_array && to.pointer_depth > 0 && from.name == to.name) {
					return value;
				}
				errors_.push_back(std::format("Cannot {} cast from {} to {}",
											  implicit ? "implicitly" : "explicitly",
											  type_to_string(from),
											  type_to_string(to)));
				return value;
			}

			llvm::Value* promote_variadic_argument(llvm::Value* value, SemanticType& type) {
				if (value == nullptr) {
					return value;
				}

				if (type.pointer_depth > 0 || type.is_reference || type.is_array || type.is_error) {
					return value;
				}

				if (type.name == "float") {
					value = builder_.CreateFPExt(value, llvm::Type::getDoubleTy(context_), "vararg.fp.extend");
					type.name = "double";
					return value;
				}

				if (is_integer_type(type)) {
					const unsigned bits = integer_bit_width(type);
					if (bits < 32) {
						SemanticType promoted = type;
						if (type.name == "bool" || type.name == "char" || type.name == "uint8" || type.name == "uint16") {
							promoted.name = "int32";
						} else {
							promoted.name = "int32";
						}
						value = builder_.CreateIntCast(value, llvm_type(promoted), is_signed_integer_type(type), "vararg.int.extend");
						type = promoted;
					}
				}

				return value;
			}

			llvm::Value* materialize_address(llvm::Value* value, const SemanticType& type, const std::string& name) {
				if (value == nullptr) {
					return nullptr;
				}
				if (type.pointer_depth > 0 || type.is_reference) {
					return value;
				}
				llvm::AllocaInst* slot = create_entry_alloca(current_function_, llvm_type(type), name);
				builder_.CreateStore(value, slot);
				return slot;
			}

			llvm::Value* as_boolean(llvm::Value* value, const SemanticType& type) {
				if (type.name == "bool" && type.pointer_depth == 0 && !type.is_reference && !type.is_array) {
					return value;
				}
				if (is_float_type(type)) {
					return builder_.CreateFCmpUNE(value, llvm::ConstantFP::get(llvm_type(type), 0.0), "bool.cast");
				}
				if (is_integer_type(type)) {
					return builder_.CreateICmpNE(value, llvm::ConstantInt::get(llvm_type(type), 0), "bool.cast");
				}
				if (type.pointer_depth > 0 || type.is_reference) {
					return builder_.CreateICmpNE(value,
												 llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)),
												 "bool.cast");
				}
				errors_.push_back(std::format("Cannot use type '{}' as condition", type_to_string(type)));
				return llvm::ConstantInt::getFalse(context_);
			}

			std::vector<SemanticType> expanded_call_arg_types(const CallExpr* call) {
				std::vector<SemanticType> arg_types;
				if (call == nullptr) {
					return arg_types;
				}
				for (const auto& argument: call->args) {
					if (const auto* unary = dynamic_cast<const UnaryExpr*>(argument.get());
						unary != nullptr && unary->op == "...") {
						if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(unary->operand.get());
							identifier != nullptr) {
							if (const auto found = current_pack_bindings_.find(identifier->name); found != current_pack_bindings_.end()) {
								for (const auto& element: found->second) {
									arg_types.push_back(element.type);
								}
								continue;
							}
						}
					}
					arg_types.push_back(infer_expr_type(argument.get()));
				}
				return arg_types;
			}

			struct ExpandedCallArgument {
				llvm::Value* value = nullptr;
				SemanticType type;
			};

			std::vector<ExpandedCallArgument> expanded_call_args(const CallExpr& call) {
				std::vector<ExpandedCallArgument> args;
				for (const auto& argument: call.args) {
					if (const auto* unary = dynamic_cast<const UnaryExpr*>(argument.get());
						unary != nullptr && unary->op == "...") {
						if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(unary->operand.get());
							identifier != nullptr) {
							if (const auto found = current_pack_bindings_.find(identifier->name); found != current_pack_bindings_.end()) {
								for (const auto& element: found->second) {
									args.push_back(ExpandedCallArgument{
										load_variable(VariableInfo{element.type, element.address, false}, identifier->name),
										element.type});
								}
								continue;
							}
						}
					}
					args.push_back(ExpandedCallArgument{emit_expression(argument.get()), infer_expr_type(argument.get())});
				}
				return args;
			}

			CallResolution resolve_call(const CallExpr* call) {
				CallResolution resolution;
				if (call == nullptr) {
					return resolution;
				}
				std::vector<SemanticType> arg_types = expanded_call_arg_types(call);

				if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(call->callee.get())) {
					if (const auto free_it = free_functions_.find(identifier->name); free_it != free_functions_.end()) {
						resolution.function = choose_overload(free_it->second, arg_types);
						if (resolution.function != nullptr) {
							if (resolution.function->is_template_instance && resolution.function->llvm_function == nullptr) {
								resolution.function = instantiate_template_function(resolution.function, arg_types);
							}
							return resolution;
						}
					}
					if (const auto templ_it = template_free_functions_.find(identifier->name); templ_it != template_free_functions_.end()) {
						for (FunctionInfo* candidate: templ_it->second) {
							if (FunctionInfo* instance = instantiate_template_function(candidate, arg_types)) {
								resolution.function = instance;
								return resolution;
							}
						}
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
							if (resolution.function != nullptr) {
								return resolution;
							}
						}
						if (const auto templ_it = template_free_functions_.find(member->member); templ_it != template_free_functions_.end()) {
							for (FunctionInfo* candidate: templ_it->second) {
								if (FunctionInfo* instance = instantiate_template_function(candidate, arg_types)) {
									resolution.function = instance;
									return resolution;
								}
							}
						}
						return resolution;
					}

					if (auto static_owner = referenced_struct_name(member->object.get())) {
						if (const auto struct_it = structs_.find(*static_owner); struct_it != structs_.end()) {
							if (const auto methods = struct_it->second.methods.find(member->member); methods != struct_it->second.methods.end()) {
								std::vector<FunctionInfo*> static_methods;
								for (FunctionInfo* function: methods->second) {
									if (function != nullptr && function->is_static_method) {
										static_methods.push_back(function);
									}
								}
								resolution.function = choose_overload(static_methods, arg_types);
								if (resolution.function == nullptr) {
									for (FunctionInfo* candidate: static_methods) {
										if (candidate == nullptr || candidate->template_params.empty() || candidate->is_template_instance) {
											continue;
										}
										if (FunctionInfo* instance = instantiate_template_function(candidate, arg_types)) {
											resolution.function = instance;
											break;
										}
									}
								}
							}
						}
						return resolution;
					}

					SemanticType object_type = infer_expr_type(member->object.get());
					if (member->via_arrow) {
						object_type.pointer_depth = 0;
						object_type.is_reference = false;
					}
					if (const auto struct_it = structs_.find(object_type.name); struct_it != structs_.end()) {
						if (const auto methods = struct_it->second.methods.find(member->member); methods != struct_it->second.methods.end()) {
							std::vector<FunctionInfo*> instance_methods;
							for (FunctionInfo* function: methods->second) {
								if (function != nullptr && !function->is_static_method) {
									instance_methods.push_back(function);
								}
							}
							resolution.function = choose_overload(instance_methods, arg_types);
							if (resolution.function == nullptr) {
								for (FunctionInfo* candidate: instance_methods) {
									if (candidate == nullptr || candidate->template_params.empty() || candidate->is_template_instance) {
										continue;
									}
									if (FunctionInfo* instance = instantiate_template_function(candidate, arg_types)) {
										resolution.function = instance;
										break;
									}
								}
							}
							if (resolution.function != nullptr) {
								resolution.object_type = object_type;
								resolution.object_address = member->via_arrow ? emit_expression(member->object.get())
																			  : ensure_address(member->object.get(), infer_expr_type(member->object.get()));
							}
						}
					}
				}
				return resolution;
			}

			template <typename Container>
			FunctionInfo* choose_overload(const Container& overloads, const std::vector<SemanticType>& args) {
				for (FunctionInfo* function: overloads) {
					if (function == nullptr) {
						continue;
					}
					if (!function->template_params.empty() && !function->is_template_instance) {
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
					std::string callee = call.callee != nullptr ? call.callee->kind() : "<null>";
					if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(call.callee.get())) {
						callee = identifier->name;
					} else if (const auto* member = dynamic_cast<const MemberExpr*>(call.callee.get())) {
						callee = member->member;
					}
					errors_.push_back(
						std::format("Unable to resolve function call '{}' at {}:{}", callee, call.location.line, call.location.column));
					return nullptr;
				}

				std::vector<llvm::Value*> args;
				llvm::Value* constructor_self_slot = nullptr;
				if ((resolution.function->kind == FunctionKind::Method && !resolution.function->is_static_method) ||
					resolution.function->kind == FunctionKind::Conversion) {
					if (resolution.object_address == nullptr) {
						errors_.push_back("Method call requires addressable object");
						return nullptr;
					}
					args.push_back(resolution.object_address);
				} else if (resolution.function->kind == FunctionKind::Constructor) {
					// Constructor expressions produce values. Initialize a temporary object and load it.
					llvm::Type* struct_type = llvm_type(SemanticType{resolution.function->owner});
					constructor_self_slot = create_entry_alloca(current_function_, struct_type, "ctor.tmp");
					args.push_back(constructor_self_slot);
				}

				const std::vector<ExpandedCallArgument> expanded_args = expanded_call_args(call);
				for (size_t i = 0; i < expanded_args.size(); ++i) {
					llvm::Value* value = expanded_args[i].value;
					SemanticType actual_type = expanded_args[i].type;
					if (value == nullptr) {
						return nullptr;
					}
					if (!resolution.function->variadic || i < resolution.function->params.size()) {
						const SemanticType& expected_type = resolution.function->params[i];
						value = cast_value(value, actual_type, expected_type, true);
					} else {
						value = promote_variadic_argument(value, actual_type);
					}
					args.push_back(value);
				}

				if (resolution.function->kind == FunctionKind::Constructor) {
					// Constructors return void and initialize the passed object in-place.
					// The expression result is the initialized value.
					builder_.CreateCall(resolution.function->llvm_function, args);
					llvm::Type* struct_type = llvm_type(SemanticType{resolution.function->owner});
					return builder_.CreateLoad(struct_type, constructor_self_slot, "ctor.value");
				}
				if (resolution.function->return_type.is_void()) {
					builder_.CreateCall(resolution.function->llvm_function, args);
					return nullptr;
				}
				return builder_.CreateCall(resolution.function->llvm_function, args, "call");
			}

			void emit_statement(const Stmt* stmt) {
				if (stmt == nullptr) {
					return;
				}
				if (const auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
					push_scope();
					for (const auto& nested: block->statements) {
						emit_statement(nested.get());
						if (builder_.GetInsertBlock()->getTerminator() != nullptr) {
							break;
						}
					}
					pop_scope();
					return;
				}
				if (const auto* expression = dynamic_cast<const ExprStmt*>(stmt)) {
					emit_expression(expression->expr.get());
					return;
				}
				if (const auto* variable = dynamic_cast<const VarDeclStmt*>(stmt)) {
					emit_var_decl(*variable);
					return;
				}
				if (const auto* return_stmt = dynamic_cast<const ReturnStmt*>(stmt)) {
					emit_return(*return_stmt);
					return;
				}
				if (const auto* yield_stmt = dynamic_cast<const YieldStmt*>(stmt)) {
					emit_yield(*yield_stmt);
					return;
				}
				if (const auto* delete_stmt = dynamic_cast<const DeleteStmt*>(stmt)) {
					emit_delete(*delete_stmt);
					return;
				}
				if (const auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
					emit_if_statement(*if_stmt);
					return;
				}
				if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
					emit_while_statement(*while_stmt);
					return;
				}
				if (const auto* for_stmt = dynamic_cast<const ForStmt*>(stmt)) {
					emit_for_statement(*for_stmt);
					return;
				}
				if (dynamic_cast<const FallthroughStmt*>(stmt) != nullptr) {
					return;
				}
				errors_.push_back(std::format("Unsupported statement kind '{}'", stmt->kind()));
			}

			void emit_var_decl(const VarDeclStmt& variable) {
				SemanticType type = from_typeref(variable.type);
				if (variable.is_static) {
					SemanticType static_type = type;
					if (variable.is_array) {
						static_type.is_array = true;
						static_type.array_size = variable.array_init.size();
					}
					StaticLocalInfo& info = ensure_static_local(variable, static_type);
					declare_variable(variable.name, VariableInfo{static_type, info.storage, false}, false);
					if (variable.init == nullptr && variable.array_init.empty()) {
						return;
					}

					llvm::Value* initialized = builder_.CreateLoad(llvm::Type::getInt1Ty(context_), info.guard, variable.name + ".static.init");
					llvm::Function* function = current_function_;
					llvm::BasicBlock* init_block = llvm::BasicBlock::Create(context_, variable.name + ".static.init.body", function);
					llvm::BasicBlock* cont_block = llvm::BasicBlock::Create(context_, variable.name + ".static.init.cont", function);
					builder_.CreateCondBr(initialized, cont_block, init_block);

					builder_.SetInsertPoint(init_block);
					if (variable.is_array) {
						SemanticType elem_type = element_type(static_type);
						for (size_t i = 0; i < variable.array_init.size(); ++i) {
							llvm::Value* init_value = emit_expression(variable.array_init[i].get());
							SemanticType init_type = infer_expr_type(variable.array_init[i].get());
							init_value = cast_value(init_value, init_type, elem_type, true);
							llvm::Value* element_address = builder_.CreateInBoundsGEP(llvm_type(static_type),
																					  info.storage,
																					  {builder_.getInt32(0), builder_.getInt32(static_cast<int>(i))},
																					  variable.name + ".static.elem");
							builder_.CreateStore(init_value, element_address);
						}
					} else if (variable.init) {
						llvm::Value* init_value = emit_expression(variable.init.get());
						SemanticType init_type = infer_expr_type(variable.init.get());
						builder_.CreateStore(cast_value(init_value, init_type, type, true), info.storage);
					}
					builder_.CreateStore(llvm::ConstantInt::getTrue(context_), info.guard);
					if (type_needs_static_cleanup(type) && info.cleanup != nullptr) {
						builder_.CreateCall(ensure_atexit(), {info.cleanup});
					}
					builder_.CreateBr(cont_block);
					builder_.SetInsertPoint(cont_block);
					return;
				}

				if (variable.is_array) {
					SemanticType array_type = type;
					array_type.is_array = true;
					array_type.array_size = variable.array_init.size();
					llvm::AllocaInst* slot = create_entry_alloca(current_function_, llvm_type(array_type), variable.name);
					declare_variable(variable.name, VariableInfo{array_type, slot, false});
					SemanticType elem_type = element_type(array_type);
					for (size_t i = 0; i < variable.array_init.size(); ++i) {
						llvm::Value* init_value = emit_expression(variable.array_init[i].get());
						SemanticType init_type = infer_expr_type(variable.array_init[i].get());
						init_value = cast_value(init_value, init_type, elem_type, true);
						llvm::Value* element_address = builder_.CreateInBoundsGEP(llvm_type(array_type),
																				  slot,
																				  {builder_.getInt32(0), builder_.getInt32(static_cast<int>(i))},
																				  variable.name + ".elem");
						builder_.CreateStore(init_value, element_address);
					}
					return;
				}

				llvm::AllocaInst* slot = create_entry_alloca(current_function_, llvm_type(type), variable.name);
				bool needs_destructor = type_has_destructor(type);
				declare_variable(variable.name, VariableInfo{type, slot, needs_destructor});
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
				for (const auto& info: owned_functions_) {
					if (info->llvm_function == function) {
						return info->return_type;
					}
				}
				return SemanticType{"void"};
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
				if (pointer == nullptr || type.pointer_depth == 0) {
					errors_.push_back("delete requires a pointer expression");
					return;
				}
				SemanticType pointee = type;
				pointee.pointer_depth = std::max(0, pointee.pointer_depth - 1);
				pointee.is_reference = false;
				llvm::Value* element_count = heap_count_from_payload(pointer);

				if (type_has_destructor(pointee)) {
					llvm::Function* function = current_function_;
					llvm::BasicBlock* cond_block = llvm::BasicBlock::Create(context_, "delete.array.cond", function);
					llvm::BasicBlock* body_block = llvm::BasicBlock::Create(context_, "delete.array.body", function);
					llvm::BasicBlock* end_block = llvm::BasicBlock::Create(context_, "delete.array.end", function);
					llvm::AllocaInst* index_slot = create_entry_alloca(current_function_, llvm::Type::getInt64Ty(context_), "delete.index");
					builder_.CreateStore(element_count, index_slot);
					builder_.CreateBr(cond_block);

					builder_.SetInsertPoint(cond_block);
					llvm::Value* index = builder_.CreateLoad(llvm::Type::getInt64Ty(context_), index_slot, "delete.index");
					llvm::Value* has_more = builder_.CreateICmpUGT(index,
																   llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0),
																   "delete.has_more");
					builder_.CreateCondBr(has_more, body_block, end_block);

					builder_.SetInsertPoint(body_block);
					llvm::Value* current = builder_.CreateSub(index,
															  llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1),
															  "delete.current");
					llvm::Value* element_address = builder_.CreateInBoundsGEP(llvm_type(pointee), pointer, current, "delete.elem.addr");
					emit_destructor_call(pointee, element_address);
					builder_.CreateStore(current, index_slot);
					builder_.CreateBr(cond_block);

					builder_.SetInsertPoint(end_block);
				}

				builder_.CreateCall(ensure_free(), {heap_raw_from_payload(pointer)});
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
				SemanticType condition_type = for_stmt.condition ? infer_expr_type(for_stmt.condition.get()) : SemanticType{"bool"};
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
				declare_variable(range_var.name, VariableInfo{index_type, index_slot, false});

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
		out << ir;
		return true;
	}

	bool LLVMBackend::write_ir_to_file(const std::string& output_path, std::ostream& err) const {
		if (!module_) {
			err << "LLVM module was not generated\n";
			return false;
		}
		std::ofstream file(output_path, std::ios::binary);
		if (!file) {
			err << "Failed to open LLVM IR output file: " << output_path << "\n";
			return false;
		}
		std::string ir;
		llvm::raw_string_ostream stream(ir);
		module_->print(stream, nullptr);
		stream.flush();
		file << ir;
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

	bool LLVMBackend::link_executable(const std::string& object_path, const std::string& output_path, std::ostream& err) const {
		// TODO: Implement linking using LLD
		// For now, just inform the user to link manually
		err << "Linking not yet implemented. Please link manually:\n";
#ifdef _WIN32
		err << "  link.exe " << object_path << " /OUT:" << output_path << " /SUBSYSTEM:CONSOLE /ENTRY:main\n";
#else
		err << "  ld " << object_path << " -o " << output_path << " -lc\n";
#endif
		return false;
	}

} // namespace dino::codegen
