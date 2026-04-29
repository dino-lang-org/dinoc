
#include "dino/frontend/sema.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <source_location>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

namespace dino::frontend {
	namespace {

		struct SemanticType {
			std::string name;
			bool is_const = false;
			bool is_nonull = false;
			int pointer_depth = 0;
			bool is_reference = false;
			bool is_array = false;
			bool is_error = false;

			static SemanticType error() {
				SemanticType t;
				t.name = "<error>";
				t.is_error = true;
				return t;
			}

			static SemanticType void_type() {
				SemanticType t;
				t.name = "void";
				return t;
			}
		};

		struct FunctionSig {
			std::string name;
			SemanticType return_type;
			std::vector<SemanticType> params;
			std::vector<bool> param_is_pack;
			std::vector<TemplateParam> template_params;
			bool variadic = false;
			bool is_extern = false;
			bool no_mangle = false;
			bool is_static = false;
			AccessModifier access = AccessModifier::Public;
			SourceLocation location;
		};

		struct FieldInfo {
			SemanticType type;
			AccessModifier access = AccessModifier::Private;
			bool is_static = false;
		};

		struct StructInfo {
			std::string name;
			std::unordered_map<std::string, FieldInfo> fields;
			std::unordered_map<std::string, FieldInfo> static_fields;
			std::unordered_map<std::string, std::vector<FunctionSig>> methods;
			std::vector<FunctionSig> constructors;
			std::vector<FunctionSig> conversions;
			bool has_destructor = false;
			SourceLocation location;
		};

		struct GlobalVarInfo {
			SemanticType type;
			AccessModifier access = AccessModifier::Private;
			bool is_extern = false;
			bool is_array = false;
			SourceLocation location;
		};

		bool is_builtin_type_name(const std::string& name) {
			static const std::unordered_map<std::string, bool> kBuiltin = {
				{"int8", true},
				{"int16", true},
				{"int32", true},
				{"int64", true},
				{"uint8", true},
				{"uint16", true},
				{"uint32", true},
				{"uint64", true},
				{"float", true},
				{"double", true},
				{"char", true},
				{"void", true},
				{"bool", true},
			};
			return kBuiltin.contains(name);
		}

		bool is_integer_type(const SemanticType& t) {
			if (t.is_error || t.pointer_depth > 0 || t.is_array) {
				return false;
			}
			return t.name == "int8" || t.name == "int16" || t.name == "int32" || t.name == "int64" || t.name == "uint8" ||
				   t.name == "uint16" || t.name == "uint32" || t.name == "uint64";
		}

		bool is_char_type(const SemanticType& t) {
			return !t.is_error && t.pointer_depth == 0 && !t.is_array && t.name == "char";
		}

		bool is_bool_type(const SemanticType& t) {
			return !t.is_error && t.pointer_depth == 0 && !t.is_array && t.name == "bool";
		}

		bool is_numeric_type(const SemanticType& t) {
			if (t.is_error || t.pointer_depth > 0 || t.is_array) {
				return false;
			}
			return is_integer_type(t) || t.name == "float" || t.name == "double";
		}

		bool is_bool_like(const SemanticType& t) {
			return t.is_error || is_bool_type(t) || is_numeric_type(t) || t.pointer_depth > 0;
		}

		bool same_type(const SemanticType& a, const SemanticType& b) {
			return a.name == b.name && a.pointer_depth == b.pointer_depth && a.is_reference == b.is_reference && a.is_array == b.is_array;
		}

		bool same_template_param_shape(const SemanticType& pattern, const SemanticType& actual) {
			return pattern.pointer_depth == actual.pointer_depth && pattern.is_reference == actual.is_reference && pattern.is_array == actual.is_array;
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

		std::string type_to_string(const SemanticType& t) {
			if (t.is_error) {
				return "<error>";
			}
			std::string out;
			if (t.is_const) {
				out += "const ";
			}
			out += t.name;
			for (int i = 0; i < t.pointer_depth; ++i) {
				out += "*";
			}
			if (t.is_reference) {
				out += "&";
			}
			if (t.is_array) {
				out += "[]";
			}
			return out;
		}

		SemanticType from_typeref(const TypeRef& ref) {
			SemanticType t;
			t.name = ref.name;
			t.is_const = ref.is_const;
			t.is_nonull = ref.is_nonull;
			t.pointer_depth = ref.pointer_depth;
			t.is_reference = ref.is_reference;
			return t;
		}

		SemanticType numeric_common_type(const SemanticType& a, const SemanticType& b) {
			if (!is_numeric_type(a) || !is_numeric_type(b)) {
				return SemanticType::error();
			}
			if (a.name == "double" || b.name == "double") {
				SemanticType t;
				t.name = "double";
				return t;
			}
			if (a.name == "float" || b.name == "float") {
				SemanticType t;
				t.name = "float";
				return t;
			}
			SemanticType t;
			t.name = "int64";
			return t;
		}

		bool is_assignable_to(const SemanticType& from, const SemanticType& to) {
			if (from.is_error || to.is_error) {
				return true;
			}
			if (same_type(from, to)) {
				return true;
			}
			if (is_numeric_type(from) && is_numeric_type(to)) {
				return true;
			}
			if (is_char_type(from) && is_char_type(to)) {
				return true;
			}
			if (is_bool_type(from) && is_bool_type(to)) {
				return true;
			}
			if (from.is_array && to.pointer_depth > 0 && from.name == to.name) {
				return true;
			}

			if (to.pointer_depth == from.pointer_depth || to.pointer_depth == from.pointer_depth + 1) {
				return true;
			}

			if (to.pointer_depth > 0 && from.name == "nullptr") {
				return true;
			}

			return false;
		}

		bool can_explicit_builtin_cast(const SemanticType& from, const SemanticType& to) {
			if (from.is_error || to.is_error) {
				return true;
			}
			if (same_type(from, to)) {
				return true;
			}
			if (is_numeric_type(from) && is_numeric_type(to)) {
				return true;
			}
			if (is_char_type(from) && is_integer_type(to)) {
				return true;
			}
			if (is_integer_type(from) && is_char_type(to)) {
				return true;
			}
			if (is_char_type(from) && (to.name == "float" || to.name == "double")) {
				return true;
			}
			if ((from.name == "float" || from.name == "double") && is_char_type(to)) {
				return true;
			}
			return false;
		}

		std::string module_alias_from_include(const std::string& include_path) {
			std::filesystem::path p(include_path);
			std::filesystem::path stem = p.stem();
			if (!stem.empty()) {
				return stem.string();
			}
			return include_path;
		}

		class TypeChecker {
		public:
			explicit TypeChecker(const std::unordered_map<std::string, std::unique_ptr<TranslationUnit>>& units)
				: units_(units) {}

			TypeCheckResult run() {
				build_globals();
				for (const auto& [_, unit]: units_) {
					current_unit_ = unit.get();
					check_unit(*unit);
				}
				return std::move(result_);
			}

		private:
			struct Scope {
				std::unordered_map<std::string, SemanticType> vars;
			};

			[[nodiscard]] static std::string template_base_name(const std::string& name) {
				const size_t lt_pos = name.find('<');
				const size_t gt_pos = name.rfind('>');
				if (lt_pos != std::string::npos && gt_pos != std::string::npos && gt_pos > lt_pos) {
					return name.substr(0, lt_pos);
				}
				return name;
			}

			[[nodiscard]] const StructInfo* find_struct_info(const std::string& name) const {
				if (const auto it = structs_.find(name); it != structs_.end()) {
					return &it->second;
				}
				if (const auto it = template_structs_.find(name); it != template_structs_.end()) {
					return &it->second;
				}
				const std::string base_name = template_base_name(name);
				if (base_name == name) {
					return nullptr;
				}
				if (const auto it = structs_.find(base_name); it != structs_.end()) {
					return &it->second;
				}
				if (const auto it = template_structs_.find(base_name); it != template_structs_.end()) {
					return &it->second;
				}
				return nullptr;
			}

			void push_scope() { scopes_.emplace_back(); }
			void pop_scope() {
				if (!scopes_.empty()) {
					scopes_.pop_back();
				}
			}

			void declare_var(const std::string& name, const SemanticType& t) {
				if (scopes_.empty()) {
					push_scope();
				}
				scopes_.back().vars[name] = t;
			}

			[[nodiscard]] std::optional<SemanticType> lookup_var(const std::string& name) const {
				for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
					const auto f = it->vars.find(name);
					if (f != it->vars.end()) {
						return f->second;
					}
				}
				if (const auto global = globals_.find(name); global != globals_.end() && is_visible_symbol(name)) {
					return global->second.type;
				}
				return std::nullopt;
			}

			void error(const std::source_location& src_location, const SourceLocation& loc, const std::string& format, auto&&... args) {
				std::string final_format_msg;
#ifndef NDEBUG
				final_format_msg += "Src=";
				final_format_msg += src_location.file_name();
				final_format_msg += ":";
				final_format_msg += std::to_string(src_location.line());
				final_format_msg += ": ";
#endif
				final_format_msg += format;
				result_.errors.push_back(ParseMessage{loc, std::vformat(final_format_msg, std::make_format_args(args...))});
			}
			void warning(const SourceLocation& loc, const std::string& format, auto&&... args) { result_.warnings.push_back(ParseMessage{loc, std::vformat(format, std::make_format_args(args...))}); }

			[[nodiscard]] bool is_visible_symbol(const std::string& name) const {
				if (current_unit_ == nullptr) {
					return true;
				}
				if (current_unit_->local_symbols.contains(name)) {
					return true;
				}
				// Also check if it's a struct or template struct
				if (structs_.contains(name) || template_structs_.contains(name)) {
					return true;
				}
				return find_struct_info(name) != nullptr;
			}

			[[nodiscard]] bool is_known_type(const SemanticType& t) const {
				if (t.is_error) {
					return true;
				}
				if (active_template_types_.contains(t.name)) {
					return true;
				}
				if (is_builtin_type_name(t.name) || structs_.contains(t.name) || template_structs_.contains(t.name)) {
					return true;
				}
				return find_struct_info(t.name) != nullptr;
			}

			void build_globals() {
				for (const auto& [_, unit]: units_) {
					for (const auto& decl: unit->declarations) {
						if (const auto* st = dynamic_cast<const StructDecl*>(decl.get())) {
							StructInfo info;
							info.name = st->name;
							info.has_destructor = !st->destructors.empty();
							info.location = st->location;
							for (const auto& f: st->fields) {
								SemanticType field_type = from_typeref(f.type);
								for (const auto& field_name: f.names) {
									FieldInfo field_info{field_type, f.access, f.is_static};
									if (f.is_static) {
										info.static_fields[field_name] = field_info;
									} else {
										info.fields[field_name] = field_info;
									}
								}
							}
							for (const auto& m: st->methods) {
								FunctionSig sig;
								sig.name = m.name;
								sig.return_type = from_typeref(m.return_type);
								sig.access = m.access;
								sig.location = m.location;
								sig.template_params = m.template_params;
								for (const auto& p: m.parameters) {
									SemanticType pt = from_typeref(p.type);
									if (p.type.variadic) {
										sig.variadic = true;
									}
									sig.param_is_pack.push_back(p.is_pack);
									sig.params.push_back(pt);
								}
								sig.is_extern = m.attributes.is_extern;
								sig.no_mangle = m.attributes.no_mangle;
								sig.is_static = m.is_static;
								info.methods[m.name].push_back(std::move(sig));
							}
							for (const auto& c: st->constructors) {
								FunctionSig sig;
								sig.name = c.name;
								sig.return_type = SemanticType{st->name};
								sig.access = c.access;
								sig.location = c.location;
								for (const auto& p: c.parameters) {
									SemanticType pt = from_typeref(p.type);
									if (p.type.variadic) {
										sig.variadic = true;
									}
									sig.param_is_pack.push_back(p.is_pack);
									sig.params.push_back(pt);
								}
								info.constructors.push_back(std::move(sig));
							}
							for (const auto& c: st->conversions) {
								FunctionSig sig;
								sig.name = "<conversion>";
								sig.return_type = from_typeref(c.target_type);
								sig.access = c.access;
								sig.location = c.location;
								info.conversions.push_back(std::move(sig));
							}
							// Store template structs separately
							if (!st->template_params.empty()) {
								template_structs_[st->name] = std::move(info);
							} else {
								structs_[st->name] = std::move(info);
							}
						} else if (const auto* fn = dynamic_cast<const FunctionDecl*>(decl.get())) {
							FunctionSig sig;
							sig.name = fn->name;
							sig.return_type = from_typeref(fn->return_type);
							sig.location = fn->location;
							sig.template_params = fn->template_params;
							for (const auto& p: fn->parameters) {
								SemanticType pt = from_typeref(p.type);
								if (p.type.variadic) {
									sig.variadic = true;
								}
								sig.param_is_pack.push_back(p.is_pack);
								sig.params.push_back(pt);
							}
							sig.is_extern = fn->attributes.is_extern;
							sig.no_mangle = fn->attributes.no_mangle;
							functions_[fn->name].push_back(std::move(sig));
						} else if (const auto* global = dynamic_cast<const GlobalVarDecl*>(decl.get())) {
							GlobalVarInfo info;
							info.type = from_typeref(global->type);
							info.type.is_array = global->is_array;
							info.access = global->access;
							info.is_extern = global->is_extern;
							info.is_array = global->is_array;
							info.location = global->location;
							globals_[global->name] = std::move(info);
						}
					}
				}
			}

			void check_unit(const TranslationUnit& unit) {
				current_modules_.clear();
				for (const auto& decl: unit.declarations) {
					if (const auto* include = dynamic_cast<const IncludeDecl*>(decl.get())) {
						current_modules_[module_alias_from_include(include->include_path)] = include->resolved_path;
					}
				}
				for (const auto& decl: unit.declarations) {
					if (const auto* fn = dynamic_cast<const FunctionDecl*>(decl.get())) {
						check_function(*fn);
					} else if (const auto* global = dynamic_cast<const GlobalVarDecl*>(decl.get())) {
						check_global(*global);
					} else if (const auto* st = dynamic_cast<const StructDecl*>(decl.get())) {
						check_struct(*st);
					}
				}
			}

			void check_struct(const StructDecl& st) {
				// Skip type checking for template structs - they will be checked when instantiated
				if (!st.template_params.empty()) {
					return;
				}
				current_struct_ = st.name;
				active_template_types_.clear();
				for (const auto& tp: st.template_params) {
					active_template_types_.insert(tp.name);
				}

				for (const auto& f: st.fields) {
					SemanticType ft = from_typeref(f.type);
					if (!is_known_type(ft) || (!is_builtin_type_name(ft.name) && !active_template_types_.contains(ft.name) &&
											   !is_visible_symbol(ft.name) && ft.name != st.name)) {
						error(std::source_location::current(), f.location, "In structure '{}': unknown type '{}' for field with name '{}'", st.name, f.type.name, ft.name);
					}
					if (f.is_static && ft.is_const && f.init == nullptr) {
						error(std::source_location::current(), f.location, "Static const field in structure '{}' requires an initializer", st.name);
					}
					if (!f.is_static && f.init != nullptr) {
						error(std::source_location::current(), f.location, "Field initializers are currently supported only for static fields");
					}
					if (f.init != nullptr) {
						SemanticType init_type = infer_expr_type(f.init.get());
						if (!is_assignable_to(init_type, ft)) {
							error(std::source_location::current(), f.location,
								  "Incompatible field initialization: {} <== {}",
								  type_to_string(ft),
								  type_to_string(init_type));
						}
					}
				}

				for (const auto& ctor: st.constructors) {
					check_constructor(st, ctor);
				}
				for (const auto& dtor: st.destructors) {
					check_destructor(st, dtor);
				}
				for (const auto& method: st.methods) {
					check_method(st, method);
				}
				for (const auto& conv: st.conversions) {
					check_conversion(st, conv);
				}

				current_struct_.reset();
				active_template_types_.clear();
			}

			void check_function(const FunctionDecl& fn) {
				active_template_types_.clear();
				for (const auto& tp: fn.template_params) {
					active_template_types_.insert(tp.name);
				}
				check_ffi_attributes(fn.location,
									 fn.name,
									 fn.attributes,
									 fn.body != nullptr,
									 functions_[fn.name].size(),
									 std::ranges::any_of(fn.parameters, [](const Parameter& parameter) { return parameter.type.variadic; }),
									 false,
									 "");
				SemanticType ret = from_typeref(fn.return_type);
				if (!is_known_type(ret) || (!is_builtin_type_name(ret.name) && !active_template_types_.contains(ret.name) &&
											!is_visible_symbol(ret.name))) {
					error(std::source_location::current(), fn.location, "Unknown return type for function '{}': '{}'", fn.name, fn.return_type.name);
				}

				push_scope();
				for (const auto& p: fn.parameters) {
					SemanticType pt = from_typeref(p.type);
					if (!is_known_type(pt) || (!is_builtin_type_name(pt.name) && !active_template_types_.contains(pt.name) &&
											   !is_visible_symbol(pt.name))) {
						error(std::source_location::current(), fn.location, "In function '{}': unknown type '{}' for parameter with name {}" + p.type.name, p.name);
					}
					if (!p.name.empty() && !p.is_pack) {
						declare_var(p.name, pt);
					}
				}

				if (!fn.template_params.empty()) {
					pop_scope();
					active_template_types_.clear();
					return;
				}

				if (fn.body != nullptr) {
					return_type_stack_.push_back(ret);
					check_statement(fn.body.get(), false, nullptr);
					return_type_stack_.pop_back();
				}
				pop_scope();
				active_template_types_.clear();
			}

			void check_global(const GlobalVarDecl& global) {
				if (global.is_extern && global.init != nullptr) {
					error(std::source_location::current(), global.location, "Extern global '{}' cannot have an initializer", global.name);
				}
				if (global.is_extern && !global.array_init.empty()) {
					error(std::source_location::current(), global.location, "Extern global '{}' cannot have an array initializer", global.name);
				}

				SemanticType global_type = from_typeref(global.type);
				global_type.is_array = global.is_array;
				if (!is_known_type(global_type) || (!is_builtin_type_name(global_type.name) && !is_visible_symbol(global_type.name))) {
					error(std::source_location::current(), global.location, "Unknown type '{}' for global variable '{}'", global.type.name, global.name);
				}

				if (global_type.is_const && !global.is_extern && global.init == nullptr && global.array_init.empty() && !global.has_brace_init) {
					error(std::source_location::current(), global.location, "Const global '{}' requires an initializer", global.name);
				}

				if (global.init) {
					SemanticType init_type = infer_expr_type(global.init.get());
					SemanticType assign_to = global_type;
					if (assign_to.is_array) {
						assign_to.is_array = false;
						assign_to.pointer_depth = 1;
					}
					if (!is_assignable_to(init_type, assign_to)) {
						error(std::source_location::current(), global.location,
							  "Incompatible global initialization: {} <== {}",
							  type_to_string(assign_to),
							  type_to_string(init_type));
					}
				}

				for (const auto& expr: global.array_init) {
					SemanticType item_type = infer_expr_type(expr.get());
					SemanticType element_type = global_type;
					element_type.is_array = false;
					if (!is_assignable_to(item_type, element_type)) {
						error(std::source_location::current(), global.location,
							  "Incompatible global array element type: {} <== {}",
							  type_to_string(element_type),
							  type_to_string(item_type));
					}
				}
			}

			void check_constructor(const StructDecl& owner, const ConstructorDecl& ctor) {
				push_scope();
				SemanticType this_type;
				this_type.name = owner.name;
				this_type.pointer_depth = 1;
				declare_var("this", this_type);

				for (const auto& p: ctor.parameters) {
					SemanticType pt = from_typeref(p.type);
					if (!is_known_type(pt) || (!is_builtin_type_name(pt.name) && !active_template_types_.contains(pt.name) &&
											   !is_visible_symbol(pt.name) && pt.name != owner.name)) {
						error(std::source_location::current(), ctor.location, "In structure '{}': unknown type '{}' for constructor parameter with name '{}'", owner.name, p.type.name, p.name);
					}
					if (!p.name.empty()) {
						declare_var(p.name, pt);
					}
				}

				return_type_stack_.push_back(SemanticType::void_type());
				check_statement(ctor.body.get(), false, nullptr);
				return_type_stack_.pop_back();
				pop_scope();
			}

			void check_destructor(const StructDecl& owner, const DestructorDecl& dtor) {
				push_scope();
				SemanticType this_type;
				this_type.name = owner.name;
				this_type.pointer_depth = 1;
				declare_var("this", this_type);

				return_type_stack_.push_back(SemanticType::void_type());
				check_statement(dtor.body.get(), false, nullptr);
				return_type_stack_.pop_back();
				pop_scope();
			}

			void check_method(const StructDecl& owner, const MethodDecl& method) {
				active_template_types_.clear();
				for (const auto& tp: owner.template_params) {
					active_template_types_.insert(tp.name);
				}
				for (const auto& tp: method.template_params) {
					active_template_types_.insert(tp.name);
				}
				check_ffi_attributes(method.location,
									 method.name,
									 method.attributes,
									 method.body != nullptr,
									 structs_[owner.name].methods[method.name].size(),
									 std::ranges::any_of(method.parameters, [](const Parameter& parameter) { return parameter.type.variadic; }),
									 true,
									 owner.name);
				SemanticType ret = from_typeref(method.return_type);
				if (!is_known_type(ret) || (!is_builtin_type_name(ret.name) && !active_template_types_.contains(ret.name) &&
											!is_visible_symbol(ret.name) && ret.name != owner.name)) {
					error(std::source_location::current(), method.location, "In structure '{}': in method '{}': unknown return type: '{}'", owner.name, method.name, method.return_type.name);
				}

				push_scope();
				current_method_is_static_ = method.is_static;
				if (!method.is_static) {
					SemanticType this_type;
					this_type.name = owner.name;
					this_type.pointer_depth = 1;
					declare_var("this", this_type);
				}

				for (const auto& p: method.parameters) {
					SemanticType pt = from_typeref(p.type);
					if (!is_known_type(pt) || (!is_builtin_type_name(pt.name) && !active_template_types_.contains(pt.name) &&
											   !is_visible_symbol(pt.name) && pt.name != owner.name)) {
						error(std::source_location::current(), method.location, "In structure '{}': in method '{}': unknown type '{}' for parameter with name '{}'", owner.name, method.name, p.type.name, p.name);
					}
					if (!p.name.empty()) {
						declare_var(p.name, pt);
					}
				}

				if (!method.template_params.empty()) {
					current_method_is_static_ = false;
					pop_scope();
					active_template_types_.clear();
					return;
				}

				if (method.body != nullptr) {
					return_type_stack_.push_back(ret);
					check_statement(method.body.get(), false, nullptr);
					return_type_stack_.pop_back();
				}
				current_method_is_static_ = false;
				pop_scope();
				active_template_types_.clear();
			}

			std::optional<std::string> referenced_struct_name(const Expr* expr) const {
				const auto* identifier = dynamic_cast<const IdentifierExpr*>(expr);
				if (identifier == nullptr) {
					return std::nullopt;
				}
				if (lookup_var(identifier->name).has_value()) {
					return std::nullopt;
				}
				if (!is_visible_symbol(identifier->name)) {
					return std::nullopt;
				}
				const StructInfo* info = find_struct_info(identifier->name);
				if (info == nullptr) {
					return std::nullopt;
				}
				return info->name;
			}

			[[nodiscard]] const FunctionSig* choose_method_overload(const std::vector<SemanticType>& args,
																	const std::vector<FunctionSig>& overloads,
																	bool want_static) const {
				for (const auto& overload: overloads) {
					if (overload.is_static != want_static) {
						continue;
					}
					if (!overload.template_params.empty()) {
						continue;
					}
					if (args_match_sig(args, overload)) {
						return &overload;
					}
				}
				return nullptr;
			}

			void check_ffi_attributes(const SourceLocation& loc,
									  const std::string& name,
									  const FunctionAttributes& attributes,
									  bool has_body,
									  size_t overload_count,
									  bool is_variadic,
									  bool is_method,
									  const std::string& owner) {
				if (attributes.is_extern && attributes.no_mangle) {
					error(std::source_location::current(), loc, "Attributes '#[extern]' and '#[no_mangle]' cannot be used together on '{}'", name);
				}
				if (attributes.is_extern && has_body) {
					error(std::source_location::current(), loc, "Extern {} '{}' must not have a body", is_method ? "method" : "function", name);
				}
				if (!attributes.is_extern && !has_body) {
					error(std::source_location::current(), loc, "Only '#[extern]' {} declarations may omit a body for '{}'", is_method ? "method" : "function", name);
				}
				if (attributes.no_mangle && !has_body) {
					error(std::source_location::current(), loc, "No-mangle {} '{}' must have a body", is_method ? "method" : "function", name);
				}
				if (attributes.uses_c_abi() && overload_count > 1) {
					if (is_method) {
						error(std::source_location::current(), loc, "Method '{}.{}' with C ABI attributes cannot be overloaded", owner, name);
					} else {
						error(std::source_location::current(), loc, "Function '{}' with C ABI attributes cannot be overloaded", name);
					}
				}
				if (is_variadic && !attributes.is_extern) {
					error(std::source_location::current(), loc,
						  "Variadic arguments are only allowed for '#[extern]' {} '{}'",
						  is_method ? "methods" : "functions",
						  name);
				}
			}

			void check_conversion(const StructDecl& owner, const ConversionDecl& conv) {
				SemanticType ret = from_typeref(conv.target_type);
				if (!is_known_type(ret) || (!is_builtin_type_name(ret.name) && !active_template_types_.contains(ret.name) &&
											!is_visible_symbol(ret.name) && ret.name != owner.name)) {
					error(std::source_location::current(), conv.location, "In structure '{}': unknown convertor type: '{}'", owner.name, conv.target_type.name);
				}

				push_scope();
				SemanticType this_type;
				this_type.name = owner.name;
				this_type.pointer_depth = 1;
				declare_var("this", this_type);

				return_type_stack_.push_back(ret);
				check_statement(conv.body.get(), false, nullptr);
				return_type_stack_.pop_back();
				pop_scope();
			}

			[[nodiscard]] const std::vector<FunctionSig>* visible_functions(const std::string& name) const {
				const auto it = functions_.find(name);
				if (it == functions_.end()) {
					return nullptr;
				}
				if (!is_visible_symbol(name)) {
					return nullptr;
				}
				return &it->second;
			}

			static bool args_match_sig(const std::vector<SemanticType>& args, const FunctionSig& sig) {
				const size_t min_required = sig.variadic && !sig.params.empty() ? sig.params.size() - 1 : sig.params.size();
				if (!sig.variadic && args.size() != sig.params.size()) {
					return false;
				}
				if (sig.variadic && args.size() < min_required) {
					return false;
				}
				const size_t check_params = sig.variadic && !sig.params.empty() ? sig.params.size() - 1 : sig.params.size();
				for (size_t i = 0; i < check_params; ++i) {
					if (!is_assignable_to(args[i], sig.params[i])) {
						return false;
					}
				}
				return true;
			}

			bool deduce_template_binding(const SemanticType& pattern,
										 const SemanticType& actual,
										 const std::unordered_set<std::string>& template_names,
										 std::unordered_map<std::string, SemanticType>& bindings) const {
				if (template_names.contains(pattern.name)) {
					SemanticType deduced = actual;
					if (!matches_template_wrapper_shape(pattern, actual)) {
						return false;
					}
					if (pattern.pointer_depth > 0) {
						deduced.pointer_depth = 0;
					}
					if (pattern.is_reference) {
						deduced.is_reference = false;
					}
					if (pattern.is_array) {
						deduced.is_array = false;
					}
					const auto found = bindings.find(pattern.name);
					if (found == bindings.end()) {
						bindings[pattern.name] = deduced;
						return true;
					}
					return same_type(found->second, deduced);
				}
				return is_assignable_to(actual, pattern);
			}

			std::optional<SemanticType> deduce_template_return_type(const FunctionSig& sig,
																	const std::unordered_map<std::string, SemanticType>& bindings) const {
				if (bindings.empty()) {
					return sig.return_type;
				}
				SemanticType resolved = sig.return_type;
				if (const auto found = bindings.find(resolved.name); found != bindings.end()) {
					SemanticType substituted = found->second;
					substituted.pointer_depth = resolved.pointer_depth;
					substituted.is_reference = resolved.is_reference;
					substituted.is_array = resolved.is_array;
					return substituted;
				}
				return resolved;
			}

			std::optional<SemanticType> try_match_template_overload(const std::vector<SemanticType>& args, const FunctionSig& sig) const {
				if (sig.template_params.empty()) {
					return std::nullopt;
				}

				std::unordered_set<std::string> template_names;
				std::unordered_set<std::string> pack_template_names;
				for (const auto& param: sig.template_params) {
					template_names.insert(param.name);
					if (param.is_pack) {
						pack_template_names.insert(param.name);
					}
				}

				const bool has_pack = std::ranges::any_of(sig.param_is_pack, [](bool value) { return value; });
				if (has_pack) {
					size_t pack_index = 0;
					while (pack_index < sig.param_is_pack.size() && !sig.param_is_pack[pack_index]) {
						++pack_index;
					}
					if (pack_index != sig.param_is_pack.size() - 1) {
						return std::nullopt;
					}
					if (args.size() < pack_index) {
						return std::nullopt;
					}
					std::unordered_map<std::string, SemanticType> bindings;
					for (size_t i = 0; i < pack_index; ++i) {
						if (!deduce_template_binding(sig.params[i], args[i], template_names, bindings)) {
							return std::nullopt;
						}
					}
					const SemanticType& pack_pattern = sig.params[pack_index];
					for (size_t i = pack_index; i < args.size(); ++i) {
						if (pack_template_names.contains(pack_pattern.name)) {
							if (!matches_template_wrapper_shape(pack_pattern, args[i])) {
								return std::nullopt;
							}
							continue;
						}
						if (!deduce_template_binding(pack_pattern, args[i], template_names, bindings)) {
							return std::nullopt;
						}
					}
					return deduce_template_return_type(sig, bindings);
				}

				if (args.size() != sig.params.size()) {
					return std::nullopt;
				}
				std::unordered_map<std::string, SemanticType> bindings;
				for (size_t i = 0; i < args.size(); ++i) {
					if (!deduce_template_binding(sig.params[i], args[i], template_names, bindings)) {
						return std::nullopt;
					}
				}
				return deduce_template_return_type(sig, bindings);
			}

			[[nodiscard]] const FunctionSig* choose_overload(const std::vector<SemanticType>& args, const std::vector<FunctionSig>& overloads) const {
				for (const auto& sig: overloads) {
					if (!sig.template_params.empty()) {
						continue;
					}
					if (args_match_sig(args, sig)) {
						return &sig;
					}
				}
				return nullptr;
			}

			[[nodiscard]] bool can_explicit_struct_cast(const SemanticType& from, const SemanticType& to) const {
				if (from.is_error || to.is_error || from.pointer_depth > 0 || from.is_array) {
					return false;
				}
				const auto it = structs_.find(from.name);
				if (it == structs_.end()) {
					return false;
				}
				for (const auto& conv: it->second.conversions) {
					if (!same_type(conv.return_type, to)) {
						continue;
					}
					if (current_struct_.has_value() && *current_struct_ == from.name) {
						return true;
					}
					if (conv.access == AccessModifier::Public) {
						return true;
					}
				}
				return false;
			}

			void check_statement(const Stmt* stmt, bool allow_yield, std::vector<SemanticType>* yielded) {
				if (stmt == nullptr) {
					return;
				}

				if (const auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
					push_scope();
					for (const auto& item: block->statements) {
						check_statement(item.get(), allow_yield, yielded);
					}
					pop_scope();
					return;
				}

				if (const auto* s = dynamic_cast<const ExprStmt*>(stmt)) {
					infer_expr_type(s->expr.get());
					return;
				}

				if (const auto* s = dynamic_cast<const ReturnStmt*>(stmt)) {
					SemanticType expected = return_type_stack_.empty() ? SemanticType::void_type() : return_type_stack_.back();
					SemanticType got = s->value ? infer_expr_type(s->value.get()) : SemanticType::void_type();
					if (!is_assignable_to(got, expected)) {
						error(std::source_location::current(), s->location, "Incompatible types for return: expected: '{}', got: '{}'", type_to_string(expected), type_to_string(got));
					}
					return;
				}

				if (const auto* s = dynamic_cast<const YieldStmt*>(stmt)) {
					SemanticType got = s->value ? infer_expr_type(s->value.get()) : SemanticType::void_type();
					if (!allow_yield) {
						error(std::source_location::current(), s->location, "Yield allowed only in if/match expressions");
					}
					if (yielded != nullptr) {
						yielded->push_back(got);
					}
					return;
				}

				if (dynamic_cast<const FallthroughStmt*>(stmt)) {
					return;
				}

				if (const auto* s = dynamic_cast<const DeleteStmt*>(stmt)) {
					SemanticType value_type = infer_expr_type(s->value.get());
					if (value_type.pointer_depth == 0) {
						error(std::source_location::current(), s->location, "delete requires a pointer expression");
					}
					return;
				}

				if (const auto* s = dynamic_cast<const VarDeclStmt*>(stmt)) {
					SemanticType var_type = from_typeref(s->type);
					var_type.is_array = s->is_array;
					if (!is_known_type(var_type) || (!is_builtin_type_name(var_type.name) && !is_visible_symbol(var_type.name))) {
						error(std::source_location::current(), s->location, "Unknown type '{}' for variable with name '{}'", s->type.name, s->name);
					}
					if (var_type.is_const && s->init == nullptr && s->array_init.empty() && !s->has_brace_init) {
						error(std::source_location::current(), s->location, "Const variable '{}' requires an initializer", s->name);
					}

					if (s->init) {
						SemanticType init_type = infer_expr_type(s->init.get());
						SemanticType assign_to = var_type;
						if (assign_to.is_array) {
							assign_to.is_array = false;
							assign_to.pointer_depth = 1;
						}
						if (!is_assignable_to(init_type, assign_to)) {
							error(std::source_location::current(), s->location,
								  "Incompatible initialization: {} <== {}", type_to_string(assign_to), type_to_string(init_type));
						}
					}
					for (const auto& e: s->array_init) {
						SemanticType item = infer_expr_type(e.get());
						SemanticType elem = var_type;
						elem.is_array = false;
						if (!is_assignable_to(item, elem)) {
							error(std::source_location::current(), s->location,
								  "Incompatible array element type: {} <== {}", type_to_string(elem), type_to_string(item));
						}
					}

					declare_var(s->name, var_type);
					return;
				}

				if (const auto* s = dynamic_cast<const IfStmt*>(stmt)) {
					SemanticType cond = infer_expr_type(s->condition.get());
					if (!is_bool_like(cond)) {
						error(std::source_location::current(), s->location, "Invalid if expression: expected bool-compatible expression");
					}
					check_statement(s->then_stmt.get(), allow_yield, yielded);
					check_statement(s->else_stmt.get(), allow_yield, yielded);
					return;
				}

				if (const auto* s = dynamic_cast<const WhileStmt*>(stmt)) {
					SemanticType cond = infer_expr_type(s->condition.get());
					if (!is_bool_like(cond)) {
						error(std::source_location::current(), s->location, "Invalid while expression: expected bool-compatible");
					}
					check_statement(s->body.get(), allow_yield, yielded);
					return;
				}

				if (const auto* s = dynamic_cast<const ForStmt*>(stmt)) {
					push_scope();
					if (s->range_var.has_value()) {
						SemanticType it_type = from_typeref(s->range_var->type);
						declare_var(s->range_var->name, it_type);
						SemanticType range_t = infer_expr_type(s->range_expr.get());
						if (!(range_t.is_array || range_t.pointer_depth > 0)) {
							warning(s->location, "for-in expects array on right side");
						}
					} else {
						check_statement(s->init.get(), false, nullptr);
						SemanticType cond = s->condition ? infer_expr_type(s->condition.get()) : SemanticType{"bool"};
						if (!is_bool_like(cond)) {
							error(std::source_location::current(), s->location, "Invalid for expression: expected bool-compatible expression");
						}
						if (s->step) {
							infer_expr_type(s->step.get());
						}
					}
					check_statement(s->body.get(), allow_yield, yielded);
					pop_scope();
					return;
				}
			}

			SemanticType infer_block_yield_type(const BlockStmt* block) {
				std::vector<SemanticType> yields;
				check_statement(block, true, &yields);
				if (yields.empty()) {
					error(std::source_location::current(), block->location, "In expression block expects yield expression");
					return SemanticType::error();
				}
				SemanticType current = yields.front();
				for (size_t i = 1; i < yields.size(); ++i) {
					if (same_type(current, yields[i])) {
						continue;
					}
					if (is_numeric_type(current) && is_numeric_type(yields[i])) {
						current = numeric_common_type(current, yields[i]);
						continue;
					}
					error(std::source_location::current(), block->location,
						  "Incompatible types for yields: '{}' and '{}'", type_to_string(current), type_to_string(yields[i]));
					return SemanticType::error();
				}
				return current;
			}

			SemanticType infer_branch_type(const std::variant<ExprPtr, std::unique_ptr<BlockStmt>>& v) {
				if (const auto* e = std::get_if<ExprPtr>(&v)) {
					return infer_expr_type(e->get());
				}
				return infer_block_yield_type(std::get<std::unique_ptr<BlockStmt>>(v).get());
			}

			SemanticType infer_expr_type(const Expr* expr) {
				if (expr == nullptr) {
					return SemanticType::void_type();
				}

				if (const auto* e = dynamic_cast<const LiteralExpr*>(expr)) {
					SemanticType t;
					if (e->literal_kind == "String") {
						t.name = "char";
						t.is_const = true;
						t.pointer_depth = 1;
						return t;
					}
					if (e->literal_kind == "Character") {
						t.name = "char";
						return t;
					}
					if (e->literal_kind == "KwTrue" || e->literal_kind == "KwFalse") {
						t.name = "bool";
						return t;
					}
					if (e->value.find('.') != std::string::npos) {
						t.name = "double";
						return t;
					}
					t.name = "int32";
					return t;
				}

				if (const auto* e = dynamic_cast<const IdentifierExpr*>(expr)) {
					if (const auto var = lookup_var(e->name)) {
						return *var;
					}
					if (current_struct_.has_value() && current_method_is_static_) {
						const auto sit = structs_.find(*current_struct_);
						if (sit != structs_.end()) {
							const auto fit = sit->second.fields.find(e->name);
							if (fit != sit->second.fields.end()) {
								error(std::source_location::current(), e->location, "Static methods cannot access instance field '{}' without an object", e->name);
								return SemanticType::error();
							}
						}
					}
					if (current_struct_.has_value()) {
						const auto sit = structs_.find(*current_struct_);
						if (sit != structs_.end()) {
							const auto sfit = sit->second.static_fields.find(e->name);
							if (sfit != sit->second.static_fields.end()) {
								return sfit->second.type;
							}
						}
					}
					if (current_struct_.has_value() && !current_method_is_static_) {
						const auto sit = structs_.find(*current_struct_);
						if (sit != structs_.end()) {
							const auto fit = sit->second.fields.find(e->name);
							if (fit != sit->second.fields.end()) {
								return fit->second.type;
							}
						}
					}
					if (current_modules_.contains(e->name)) {
						SemanticType t;
						t.name = "<module>";
						return t;
					}
					if (visible_functions(e->name) != nullptr) {
						SemanticType t;
						t.name = "<function>";
						return t;
					}
					if (structs_.contains(e->name) && is_visible_symbol(e->name)) {
						SemanticType t;
						t.name = "<type>";
						return t;
					}
					error(std::source_location::current(), e->location, "Unknown identifier: {}", e->name);
					return SemanticType::error();
				}

				if (const auto* e = dynamic_cast<const UnaryExpr*>(expr)) {
					SemanticType operand = infer_expr_type(e->operand.get());
					if (e->op == "...") {
						return operand;
					}
					if (e->op == "++" || e->op == "--") {
						if (!is_numeric_type(operand)) {
							error(std::source_location::current(), e->location, "Operator {} requires numeric operand", e->op);
							return SemanticType::error();
						}
						return operand;
					}
					if (e->op == "+" || e->op == "-") {
						if (!is_numeric_type(operand)) {
							error(std::source_location::current(), e->location, "Operator {} requires numeric operand", e->op);
							return SemanticType::error();
						}
						return operand;
					}
					if (e->op == "!") {
						if (!is_bool_like(operand)) {
							error(std::source_location::current(), e->location, "Operator {} requires bool-compatible operand", e->op);
							return SemanticType::error();
						}
						return SemanticType{"bool"};
					}
					if (e->op == "~") {
						if (!is_integer_type(operand)) {
							error(std::source_location::current(), e->location, "Operator {} requires integer operand", e->op);
							return SemanticType::error();
						}
						return operand;
					}
					if (e->op == "*") {
						if (operand.pointer_depth == 0 && !operand.is_array) {
							error(std::source_location::current(), e->location, "Operator {} requires array/pointer", e->op);
							return SemanticType::error();
						}
						operand.is_array = false;
						operand.pointer_depth = 0;
						return operand;
					}
					if (e->op == "&") {
						if (operand.is_error) {
							return operand;
						}
						operand.pointer_depth = 1;
						operand.is_array = false;
						return operand;
					}
					return operand;
				}

				if (const auto* e = dynamic_cast<const BinaryExpr*>(expr)) {
					SemanticType lhs = infer_expr_type(e->lhs.get());
					SemanticType rhs = infer_expr_type(e->rhs.get());

					if (e->op == "=" || e->op == "+=" || e->op == "-=" || e->op == "*=" || e->op == "/=" || e->op == "%=" ||
						e->op == "&=" || e->op == "|=" || e->op == "^=" || e->op == "<<=" || e->op == ">>=") {
						if (lhs.is_const) {
							error(std::source_location::current(), e->location, "Cannot assign to const object of type '{}'", type_to_string(lhs));
							return SemanticType::error();
						}
						if (!is_assignable_to(rhs, lhs)) {
							error(std::source_location::current(), e->location,
								  "Incompatible assignment: {} = {}", type_to_string(lhs), type_to_string(rhs));
							return SemanticType::error();
						}
						return lhs;
					}

					if (e->op == "+" || e->op == "-" || e->op == "*" || e->op == "/" || e->op == "%") {
						if (!((e->op == "+" || e->op == "-") && lhs.pointer_depth > 0) && (!is_numeric_type(lhs) || !is_numeric_type(rhs))) {
							error(std::source_location::current(), e->location, "Arithmetic operator requires numeric operands");
							return SemanticType::error();
						}
						return numeric_common_type(lhs, rhs);
					}

					if (e->op == "<" || e->op == ">" || e->op == "<=" || e->op == ">=") {
						if (!((is_numeric_type(lhs) && is_numeric_type(rhs)) || same_type(lhs, rhs))) {
							error(std::source_location::current(), e->location, "Comparison operator requires compatible types");
							return SemanticType::error();
						}
						return SemanticType{"bool"};
					}

					if (e->op == "==" || e->op == "!=") {
						if (!((is_numeric_type(lhs) && is_numeric_type(rhs)) || same_type(lhs, rhs))) {
							error(std::source_location::current(), e->location, "Equality operator requires compatible types");
							return SemanticType::error();
						}
						return SemanticType{"bool"};
					}

					if (e->op == "&&" || e->op == "||") {
						if (!is_bool_like(lhs) || !is_bool_like(rhs)) {
							error(std::source_location::current(), e->location, "Logical operator requires bool-compatible operands");
							return SemanticType::error();
						}
						return SemanticType{"bool"};
					}

					if (e->op == "&" || e->op == "|" || e->op == "^") {
						if (!is_integer_type(lhs) || !is_integer_type(rhs)) {
							error(std::source_location::current(), e->location, "Byte-each operator requires integer operands");
							return SemanticType::error();
						}
						return numeric_common_type(lhs, rhs);
					}

					if (e->op == "<<" || e->op == ">>") {
						if (!is_integer_type(lhs) || !is_integer_type(rhs)) {
							error(std::source_location::current(), e->location, "Shift operator requires integer types");
							return SemanticType::error();
						}
						return lhs;
					}

					return SemanticType::error();
				}

				if (const auto* e = dynamic_cast<const MemberExpr*>(expr)) {
					if (const auto* mod = dynamic_cast<const IdentifierExpr*>(e->object.get())) {
						const auto mit = current_modules_.find(mod->name);
						if (mit != current_modules_.end()) {
							const auto uit = units_.find(mit->second);
							if (uit != units_.end() && uit->second->exported_symbols.contains(e->member)) {
								if (const auto global = globals_.find(e->member); global != globals_.end()) {
									return global->second.type;
								}
							}
						}
					}
					if (auto static_owner = referenced_struct_name(e->object.get())) {
						if (e->via_arrow) {
							error(std::source_location::current(), e->location, "Access operator '->' cannot be used with a type name");
							return SemanticType::error();
						}
						const StructInfo* struct_info = find_struct_info(*static_owner);
						if (struct_info == nullptr) {
							error(std::source_location::current(), e->location, "<?> Not found structure with name '{}'", *static_owner);
							return SemanticType::error();
						}
						const auto field = struct_info->static_fields.find(e->member);
						if (field != struct_info->static_fields.end()) {
							return field->second.type;
						}
						const auto method = struct_info->methods.find(e->member);
						if (method != struct_info->methods.end()) {
							for (const auto& overload: method->second) {
								if (overload.is_static) {
									warning(e->location, "Maybe you want to call it?");
									return overload.return_type;
								}
							}
							error(std::source_location::current(), e->location, "Method '{}.{}' is not static", *static_owner, e->member);
							return SemanticType::error();
						}
						error(std::source_location::current(), e->location, "Static member access supports only static fields and static methods in structure '{}'", *static_owner);
						return SemanticType::error();
					}
					SemanticType obj = infer_expr_type(e->object.get());
					if (obj.is_error) {
						return obj;
					}

					SemanticType base = obj;
					if (e->via_arrow) {
						if (base.pointer_depth == 0) {
							error(std::source_location::current(), e->location, "Access operator '->' requires pointer type");
							return SemanticType::error();
						}
						base.pointer_depth = 0;
					}
					if (base.is_array) {
						base.is_array = false;
					}

						const StructInfo* struct_info = find_struct_info(base.name);
						if (struct_info == nullptr) {
						error(std::source_location::current(), e->location, "<?> Not found structure with name '{}'", base.name);
						return SemanticType::error();
					}

						const auto field = struct_info->fields.find(e->member);
						if (field != struct_info->fields.end()) {
						return field->second.type;
					}

						const auto method = struct_info->methods.find(e->member);
						if (method != struct_info->methods.end() && !method->second.empty()) {
						for (const auto& overload: method->second) {
							if (!overload.is_static) {
								warning(e->location, "Maybe you want to call it?");
								return overload.return_type;
							}
						}
						error(std::source_location::current(), e->location, "Method '{}.{}' is static and should be called through the type", struct_info->name, e->member);
						return SemanticType::error();
					}

					error(std::source_location::current(), e->location, "In structure: '{}': unknown member with name '{}'", struct_info->name, e->member);
					return SemanticType::error();
				}

				if (const auto* e = dynamic_cast<const IndexExpr*>(expr)) {
					SemanticType obj = infer_expr_type(e->object.get());
					SemanticType idx = infer_expr_type(e->index.get());
					if (!is_integer_type(idx)) {
						error(std::source_location::current(), e->location, "Index should be integer");
					}
					if (!(obj.is_array || obj.pointer_depth > 0)) {
						error(std::source_location::current(), e->location, "Indexation requires compatible type");
						return SemanticType::error();
					}
					obj.is_array = false;
					obj.pointer_depth = 0;
					return obj;
				}

				if (const auto* e = dynamic_cast<const TypeCastExpr*>(expr)) {
					SemanticType from = infer_expr_type(e->value.get());
					SemanticType to = from_typeref(e->target_type);
					if (!is_known_type(to) || (!is_builtin_type_name(to.name) && !active_template_types_.contains(to.name) &&
											   !is_visible_symbol(to.name))) {
						error(std::source_location::current(), e->location, "Unknown target type '{}' in type_cast", e->target_type.name);
						return SemanticType::error();
					}
					if (can_explicit_builtin_cast(from, to) || can_explicit_struct_cast(from, to)) {
						return to;
					}
					error(std::source_location::current(), e->location, "type_cast from {} to {} is not allowed", type_to_string(from), type_to_string(to));
					return SemanticType::error();
				}

				if (const auto* e = dynamic_cast<const NewExpr*>(expr)) {
					SemanticType allocated = from_typeref(e->target_type);
					if (!is_known_type(allocated) || (!is_builtin_type_name(allocated.name) && !active_template_types_.contains(allocated.name) &&
													  !is_visible_symbol(allocated.name))) {
						error(std::source_location::current(), e->location, "Unknown target type '{}' in new expression", e->target_type.name);
						return SemanticType::error();
					}

					if (e->placement) {
						SemanticType placement = infer_expr_type(e->placement.get());
						if (placement.pointer_depth == 0) {
							error(std::source_location::current(), e->location, "placement new requires a pointer address");
							return SemanticType::error();
						}
						SemanticType pointee = placement;
						pointee.pointer_depth = 0;
						pointee.is_reference = false;
						if (!same_type(pointee, allocated)) {
							error(std::source_location::current(), e->location,
								  "placement new address type '{}' is incompatible with allocated type '{}'",
								  type_to_string(placement),
								  type_to_string(allocated));
							return SemanticType::error();
						}
					}

					if (e->is_array) {
						SemanticType size_type = infer_expr_type(e->array_size.get());
						if (!is_integer_type(size_type)) {
							error(std::source_location::current(), e->location, "new[] requires an integer size expression");
							return SemanticType::error();
						}
					}

					std::vector<SemanticType> args;
					args.reserve(e->args.size());
					for (const auto& arg: e->args) {
						args.push_back(infer_expr_type(arg.get()));
					}

					if (!is_builtin_type_name(allocated.name)) {
						const auto it = structs_.find(allocated.name);
						if (it == structs_.end()) {
							error(std::source_location::current(), e->location, "Cannot allocate unknown structure '{}'", allocated.name);
							return SemanticType::error();
						}
						if (!it->second.constructors.empty() && choose_overload(args, it->second.constructors) == nullptr) {
							error(std::source_location::current(), e->location, "Not found compatible constructor for structure '{}'", allocated.name);
							return SemanticType::error();
						}
					} else if (args.size() > 1) {
						error(std::source_location::current(), e->location, "Builtin type allocation supports at most one initializer argument");
						return SemanticType::error();
					}

					allocated.pointer_depth = 1;
					return allocated;
				}

				if (const auto* e = dynamic_cast<const DestructorCallExpr*>(expr)) {
					SemanticType object = infer_expr_type(e->object.get());
					SemanticType base = object;
					if (e->via_arrow) {
						if (base.pointer_depth == 0) {
							error(std::source_location::current(), e->location, "Destructor call via '->' requires pointer type");
							return SemanticType::error();
						}
						base.pointer_depth = 0;
						base.is_reference = false;
					} else if (base.is_reference) {
						base.is_reference = false;
					}

					if (base.name != e->type_name) {
						error(std::source_location::current(), e->location,
							  "Destructor name '{}' does not match object type '{}'",
							  e->type_name,
							  type_to_string(base));
						return SemanticType::error();
					}

					const StructInfo* struct_info = find_struct_info(base.name);
					if (struct_info == nullptr) {
						error(std::source_location::current(), e->location, "Explicit destructor call requires a structure type");
						return SemanticType::error();
					}
					if (!struct_info->has_destructor) {
						error(std::source_location::current(), e->location, "Structure '{}' does not have a destructor", base.name);
						return SemanticType::error();
					}
					return SemanticType::void_type();
				}

				if (const auto* e = dynamic_cast<const CallExpr*>(expr)) {
					std::vector<SemanticType> args;
					args.reserve(e->args.size());
					for (const auto& a: e->args) {
						args.push_back(infer_expr_type(a.get()));
					}

					if (const auto* callee_id = dynamic_cast<const IdentifierExpr*>(e->callee.get())) {
						if (const auto* fns = visible_functions(callee_id->name)) {
							if (const FunctionSig* sig = choose_overload(args, *fns)) {
								return sig->return_type;
							}
							for (const auto& sig: *fns) {
								if (const auto deduced = try_match_template_overload(args, sig)) {
									return *deduced;
								}
							}
							error(std::source_location::current(), e->location, "Not found compatible overload for function with name '{}'", callee_id->name);
							return SemanticType::error();
						}
						// Check if this is a template instantiation like Array<int32>
						if (const StructInfo* struct_info = find_struct_info(callee_id->name); struct_info != nullptr && is_visible_symbol(callee_id->name)) {
							const auto& ctors = struct_info->constructors;
							if (!ctors.empty() && choose_overload(args, ctors) == nullptr) {
								error(std::source_location::current(), e->location, "Not found compatible constructor for structure '{}'" + callee_id->name);
								return SemanticType::error();
							}
							SemanticType t;
							t.name = callee_id->name; // Return the full instantiation name
							return t;
						}
						error(std::source_location::current(), e->location, "Call to undefined function/constructor: {}", callee_id->name);
						return SemanticType::error();
					}

					if (const auto* member = dynamic_cast<const MemberExpr*>(e->callee.get())) {
						if (const auto* mod = dynamic_cast<const IdentifierExpr*>(member->object.get())) {
							const auto mit = current_modules_.find(mod->name);
							if (mit != current_modules_.end()) {
								const auto uit = units_.find(mit->second);
								if (uit != units_.end() && uit->second->exported_symbols.contains(member->member)) {
									if (const auto* fns = visible_functions(member->member)) {
										if (const FunctionSig* sig = choose_overload(args, *fns)) {
											return sig->return_type;
										}
										for (const auto& sig: *fns) {
											if (const auto deduced = try_match_template_overload(args, sig)) {
												return *deduced;
											}
										}
									}
									error(std::source_location::current(), member->location, "<?> Not found compatible overload for function from module '{}'", member->member);
									return SemanticType::error();
								}
								error(std::source_location::current(), member->location, "File '{}' does not export symbol with name '{}'", mod->name, member->member);
								return SemanticType::error();
							}
						}

						if (auto static_owner = referenced_struct_name(member->object.get())) {
							if (member->via_arrow) {
								error(std::source_location::current(), member->location, "Operator '->' cannot be used with a type name");
								return SemanticType::error();
							}
							const StructInfo* struct_info = find_struct_info(*static_owner);
							if (struct_info == nullptr) {
								error(std::source_location::current(), member->location, "Not found structure with name '{}'", *static_owner);
								return SemanticType::error();
							}
							const auto mit = struct_info->methods.find(member->member);
							if (mit == struct_info->methods.end()) {
								error(std::source_location::current(), member->location, "Not found static method with name '{}' in structure '{}'", member->member, *static_owner);
								return SemanticType::error();
							}
							if (const FunctionSig* sig = choose_method_overload(args, mit->second, true)) {
								return sig->return_type;
							}
							for (const auto& sig: mit->second) {
								if (!sig.is_static) {
									continue;
								}
								if (const auto deduced = try_match_template_overload(args, sig)) {
									return *deduced;
								}
							}
							error(std::source_location::current(), member->location, "Not found compatible static method overload with name '{}'", member->member);
							return SemanticType::error();
						}

						SemanticType obj = infer_expr_type(member->object.get());
						if (obj.is_error) {
							return obj;
						}
						SemanticType base = obj;
						if (member->via_arrow) {
							if (base.pointer_depth == 0) {
								error(std::source_location::current(), member->location, "Operator '->' requires pointer type");
								return SemanticType::error();
							}
							base.pointer_depth = 0;
						}
						if (base.is_array) {
							base.is_array = false;
						}

						const StructInfo* struct_info = find_struct_info(base.name);
						if (struct_info == nullptr) {
							error(std::source_location::current(), member->location, "Method calling available only for structures");
							return SemanticType::error();
						}
						const auto mit = struct_info->methods.find(member->member);
						if (mit == struct_info->methods.end()) {
							error(std::source_location::current(), member->location, "Not found methods with name '{}' in structure with name '{}'", member->member, struct_info->name);
							return SemanticType::error();
						}
						if (const FunctionSig* sig = choose_method_overload(args, mit->second, false)) {
							return sig->return_type;
						}
						for (const auto& sig: mit->second) {
							if (sig.is_static) {
								continue;
							}
							if (const auto deduced = try_match_template_overload(args, sig)) {
								return *deduced;
							}
						}
						error(std::source_location::current(), member->location, "Not found compatible method overload with name '{}'", member->member);
						return SemanticType::error();
					}

					SemanticType callee_t = infer_expr_type(e->callee.get());
					(void)callee_t;
					error(std::source_location::current(), e->location, "Call supported only for named methods/constructors");
					return SemanticType::error();
				}

				if (const auto* e = dynamic_cast<const IfExpr*>(expr)) {
					SemanticType cond = infer_expr_type(e->condition.get());
					if (!is_bool_like(cond)) {
						error(std::source_location::current(), e->location, "If expression condition should be bool-compatible");
					}

					SemanticType then_t = infer_branch_type(e->then_branch);
					if (!e->else_branch.has_value()) {
						warning(e->location, "If-else assignment should contains else branch");
						return then_t;
					}
					SemanticType else_t = infer_branch_type(*e->else_branch);
					if (same_type(then_t, else_t)) {
						return then_t;
					}
					if (is_numeric_type(then_t) && is_numeric_type(else_t)) {
						return numeric_common_type(then_t, else_t);
					}
					error(std::source_location::current(), e->location,
						  "If expression nodes contains incompatible types: {} and {}", type_to_string(then_t), type_to_string(else_t));
					return SemanticType::error();
				}

				if (const auto* e = dynamic_cast<const MatchExpr*>(expr)) {
					SemanticType subj = infer_expr_type(e->subject.get());
					std::vector<SemanticType> result_types;

					for (const auto& c: e->cases) {
						if (!c.is_default && c.match_expr) {
							SemanticType case_t = infer_expr_type(c.match_expr.get());
							if (!((is_numeric_type(case_t) && is_numeric_type(subj)) || same_type(case_t, subj))) {
								error(std::source_location::current(), c.location, "Case condition type '{}' is not compatible with match condition type '{}'", type_to_string(case_t), type_to_string(subj));
							}
						}

						if (c.fallthrough) {
							continue;
						}

						if (const auto* ce = std::get_if<ExprPtr>(&c.body)) {
							result_types.push_back(infer_expr_type(ce->get()));
						} else {
							const auto* block = std::get<std::unique_ptr<BlockStmt>>(c.body).get();
							result_types.push_back(infer_block_yield_type(block));
						}
					}

					if (result_types.empty()) {
						error(std::source_location::current(), e->location, "Match expression does not contains returning cases");
						return SemanticType::error();
					}

					SemanticType current = result_types.front();
					for (size_t i = 1; i < result_types.size(); ++i) {
						if (same_type(current, result_types[i])) {
							continue;
						}
						if (is_numeric_type(current) && is_numeric_type(result_types[i])) {
							current = numeric_common_type(current, result_types[i]);
							continue;
						}
						error(std::source_location::current(), e->location, "Match cases contains incompatible types");
						return SemanticType::error();
					}
					return current;
				}

				return SemanticType::error();
			}

		private:
			const std::unordered_map<std::string, std::unique_ptr<TranslationUnit>>& units_;
			TypeCheckResult result_;

			const TranslationUnit* current_unit_ = nullptr;
			std::optional<std::string> current_struct_;
			std::unordered_set<std::string> active_template_types_;
			std::unordered_map<std::string, std::string> current_modules_;
			std::vector<SemanticType> return_type_stack_;
			std::vector<Scope> scopes_;
			bool current_method_is_static_ = false;

			std::unordered_map<std::string, StructInfo> structs_;
			std::unordered_map<std::string, StructInfo> template_structs_;
			std::unordered_map<std::string, std::vector<FunctionSig>> functions_;
			std::unordered_map<std::string, GlobalVarInfo> globals_;
		};

	} // namespace

	TypeCheckResult type_check(const std::unordered_map<std::string, std::unique_ptr<TranslationUnit>>& units) {
		TypeChecker checker(units);
		return checker.run();
	}

} // namespace dino::frontend
