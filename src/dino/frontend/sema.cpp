
#include "dino/frontend/sema.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <variant>

namespace dino::frontend {
namespace {

struct SemanticType {
    std::string name;
    bool is_const = false;
    bool is_pointer = false;
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
    bool variadic = false;
    AccessModifier access = AccessModifier::Public;
    SourceLocation location;
};

struct FieldInfo {
    SemanticType type;
    AccessModifier access = AccessModifier::Private;
};

struct StructInfo {
    std::string name;
    std::unordered_map<std::string, FieldInfo> fields;
    std::unordered_map<std::string, std::vector<FunctionSig>> methods;
    std::vector<FunctionSig> constructors;
    std::vector<FunctionSig> conversions;
    SourceLocation location;
};

bool is_builtin_type_name(const std::string& name) {
    static const std::unordered_map<std::string, bool> kBuiltin = {
        {"int8", true},   {"int16", true}, {"int32", true},  {"int64", true},
        {"uint8", true},  {"uint16", true}, {"uint32", true}, {"uint64", true},
        {"float", true},  {"double", true}, {"char", true},   {"void", true},
        {"bool", true},
    };
    return kBuiltin.contains(name);
}

bool is_integer_type(const SemanticType& t) {
    if (t.is_error || t.is_pointer || t.is_array) {
        return false;
    }
    return t.name == "int8" || t.name == "int16" || t.name == "int32" || t.name == "int64" || t.name == "uint8" ||
           t.name == "uint16" || t.name == "uint32" || t.name == "uint64";
}

bool is_char_type(const SemanticType& t) {
    return !t.is_error && !t.is_pointer && !t.is_array && t.name == "char";
}

bool is_bool_type(const SemanticType& t) {
    return !t.is_error && !t.is_pointer && !t.is_array && t.name == "bool";
}

bool is_numeric_type(const SemanticType& t) {
    if (t.is_error || t.is_pointer || t.is_array) {
        return false;
    }
    return is_integer_type(t) || t.name == "float" || t.name == "double";
}

bool is_bool_like(const SemanticType& t) {
    return t.is_error || is_bool_type(t) || is_numeric_type(t) || t.is_pointer;
}

bool same_type(const SemanticType& a, const SemanticType& b) {
    return a.name == b.name && a.is_pointer == b.is_pointer && a.is_reference == b.is_reference && a.is_array == b.is_array;
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
    if (t.is_pointer) {
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
    t.is_pointer = ref.is_pointer;
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
    if (from.is_array && to.is_pointer && from.name == to.name) {
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
        for (const auto& [_, unit] : units_) {
            current_unit_ = unit.get();
            check_unit(*unit);
        }
        return std::move(result_);
    }

private:
    struct Scope {
        std::unordered_map<std::string, SemanticType> vars;
    };

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
        return std::nullopt;
    }

    void error(const SourceLocation& loc, const std::string& format, auto&&... args) { result_.errors.push_back(ParseMessage {loc, std::vformat(format, std::make_format_args(args...))}); }
    void warning(const SourceLocation& loc, const std::string& format, auto&&... args) { result_.warnings.push_back(ParseMessage {loc, std::vformat(format, std::make_format_args(args...))}); }

    [[nodiscard]] bool is_visible_symbol(const std::string& name) const {
        if (current_unit_ == nullptr) {
            return true;
        }
        return current_unit_->local_symbols.contains(name);
    }

    [[nodiscard]] bool is_known_type(const SemanticType& t) const {
        if (t.is_error) {
            return true;
        }
        if (active_template_types_.contains(t.name)) {
            return true;
        }
        return is_builtin_type_name(t.name) || structs_.contains(t.name);
    }

    void build_globals() {
        for (const auto& [_, unit] : units_) {
            for (const auto& decl : unit->declarations) {
                if (const auto* st = dynamic_cast<const StructDecl*>(decl.get())) {
                    StructInfo info;
                    info.name = st->name;
                    info.location = st->location;
                    for (const auto& f : st->fields) {
                        SemanticType field_type = from_typeref(f.type);
                        for (const auto& field_name : f.names) {
                            info.fields[field_name] = FieldInfo {field_type, f.access};
                        }
                    }
                    for (const auto& m : st->methods) {
                        FunctionSig sig;
                        sig.name = m.name;
                        sig.return_type = from_typeref(m.return_type);
                        sig.access = m.access;
                        sig.location = m.location;
                        for (const auto& p : m.parameters) {
                            SemanticType pt = from_typeref(p.type);
                            if (p.type.variadic) {
                                sig.variadic = true;
                            }
                            sig.params.push_back(pt);
                        }
                        info.methods[m.name].push_back(std::move(sig));
                    }
                    for (const auto& c : st->constructors) {
                        FunctionSig sig;
                        sig.name = c.name;
                        sig.return_type = SemanticType {st->name};
                        sig.access = c.access;
                        sig.location = c.location;
                        for (const auto& p : c.parameters) {
                            SemanticType pt = from_typeref(p.type);
                            if (p.type.variadic) {
                                sig.variadic = true;
                            }
                            sig.params.push_back(pt);
                        }
                        info.constructors.push_back(std::move(sig));
                    }
                    for (const auto& c : st->conversions) {
                        FunctionSig sig;
                        sig.name = "<conversion>";
                        sig.return_type = from_typeref(c.target_type);
                        sig.access = c.access;
                        sig.location = c.location;
                        info.conversions.push_back(std::move(sig));
                    }
                    structs_[st->name] = std::move(info);
                } else if (const auto* fn = dynamic_cast<const FunctionDecl*>(decl.get())) {
                    FunctionSig sig;
                    sig.name = fn->name;
                    sig.return_type = from_typeref(fn->return_type);
                    sig.location = fn->location;
                    for (const auto& p : fn->parameters) {
                        SemanticType pt = from_typeref(p.type);
                        if (p.type.variadic) {
                            sig.variadic = true;
                        }
                        sig.params.push_back(pt);
                    }
                    functions_[fn->name].push_back(std::move(sig));
                }
            }
        }
    }

    void check_unit(const TranslationUnit& unit) {
        current_modules_.clear();
        for (const auto& decl : unit.declarations) {
            if (const auto* include = dynamic_cast<const IncludeDecl*>(decl.get())) {
                current_modules_[module_alias_from_include(include->include_path)] = include->resolved_path;
            }
        }
        for (const auto& decl : unit.declarations) {
            if (const auto* fn = dynamic_cast<const FunctionDecl*>(decl.get())) {
                check_function(*fn);
            } else if (const auto* st = dynamic_cast<const StructDecl*>(decl.get())) {
                check_struct(*st);
            }
        }
    }

    void check_struct(const StructDecl& st) {
        current_struct_ = st.name;
        active_template_types_.clear();
        for (const auto& tp : st.template_params) {
            active_template_types_.insert(tp);
        }

        for (const auto& f : st.fields) {
            SemanticType ft = from_typeref(f.type);
            if (!is_known_type(ft) || (!is_builtin_type_name(ft.name) && !active_template_types_.contains(ft.name) &&
                                       !is_visible_symbol(ft.name) && ft.name != st.name)) {
                error(f.location, "In structure '{}': unknown type '{}' for field with name '{}'", st.name, f.type.name, ft.name);
            }
        }

        for (const auto& ctor : st.constructors) {
            check_constructor(st, ctor);
        }
        for (const auto& dtor : st.destructors) {
            check_destructor(st, dtor);
        }
        for (const auto& method : st.methods) {
            check_method(st, method);
        }
        for (const auto& conv : st.conversions) {
            check_conversion(st, conv);
        }

        current_struct_.reset();
        active_template_types_.clear();
    }

    void check_function(const FunctionDecl& fn) {
        active_template_types_.clear();
        for (const auto& tp : fn.template_params) {
            active_template_types_.insert(tp);
        }
        SemanticType ret = from_typeref(fn.return_type);
        if (!is_known_type(ret) || (!is_builtin_type_name(ret.name) && !active_template_types_.contains(ret.name) &&
                                    !is_visible_symbol(ret.name))) {
            error(fn.location, "Unknown return type for function '{}': '{}'", fn.name, fn.return_type.name);
        }

        push_scope();
        for (const auto& p : fn.parameters) {
            SemanticType pt = from_typeref(p.type);
            if (!is_known_type(pt) || (!is_builtin_type_name(pt.name) && !active_template_types_.contains(pt.name) &&
                                       !is_visible_symbol(pt.name))) {
                error(fn.location, "In function '{}': unknown type '{}' for parameter with name {}" + p.type.name, p.name);
            }
            if (!p.name.empty()) {
                declare_var(p.name, pt);
            }
        }

        return_type_stack_.push_back(ret);
        check_statement(fn.body.get(), false, nullptr);
        return_type_stack_.pop_back();
        pop_scope();
        active_template_types_.clear();
    }

    void check_constructor(const StructDecl& owner, const ConstructorDecl& ctor) {
        push_scope();
        SemanticType this_type;
        this_type.name = owner.name;
        this_type.is_pointer = true;
        declare_var("this", this_type);

        for (const auto& p : ctor.parameters) {
            SemanticType pt = from_typeref(p.type);
            if (!is_known_type(pt) || (!is_builtin_type_name(pt.name) && !active_template_types_.contains(pt.name) &&
                                       !is_visible_symbol(pt.name) && pt.name != owner.name)) {
                error(ctor.location, "In structure '{}': unknown type '{}' for constructor parameter with name '{}'", owner.name, p.type.name, p.name);
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
        this_type.is_pointer = true;
        declare_var("this", this_type);

        return_type_stack_.push_back(SemanticType::void_type());
        check_statement(dtor.body.get(), false, nullptr);
        return_type_stack_.pop_back();
        pop_scope();
    }

    void check_method(const StructDecl& owner, const MethodDecl& method) {
        SemanticType ret = from_typeref(method.return_type);
        if (!is_known_type(ret) || (!is_builtin_type_name(ret.name) && !active_template_types_.contains(ret.name) &&
                                    !is_visible_symbol(ret.name) && ret.name != owner.name)) {
            error(method.location, "In structure '{}': in method '{}': unknown return type: '{}'", owner.name, method.name, method.return_type.name);
        }

        push_scope();
        SemanticType this_type;
        this_type.name = owner.name;
        this_type.is_pointer = true;
        declare_var("this", this_type);

        for (const auto& p : method.parameters) {
            SemanticType pt = from_typeref(p.type);
            if (!is_known_type(pt) || (!is_builtin_type_name(pt.name) && !active_template_types_.contains(pt.name) &&
                                       !is_visible_symbol(pt.name) && pt.name != owner.name)) {
                error(method.location, "In structure '{}': in method '{}': unknown type '{}' for parameter with name '{}'", owner.name, method.name, p.type.name, p.name);
            }
            if (!p.name.empty()) {
                declare_var(p.name, pt);
            }
        }

        return_type_stack_.push_back(ret);
        check_statement(method.body.get(), false, nullptr);
        return_type_stack_.pop_back();
        pop_scope();
    }

    void check_conversion(const StructDecl& owner, const ConversionDecl& conv) {
        SemanticType ret = from_typeref(conv.target_type);
        if (!is_known_type(ret) || (!is_builtin_type_name(ret.name) && !active_template_types_.contains(ret.name) &&
                                    !is_visible_symbol(ret.name) && ret.name != owner.name)) {
            error(conv.location, "In structure '{}': unknown convertor type: '{}'", owner.name, conv.target_type.name);
        }

        push_scope();
        SemanticType this_type;
        this_type.name = owner.name;
        this_type.is_pointer = true;
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

    [[nodiscard]] const FunctionSig* choose_overload(const std::vector<SemanticType>& args, const std::vector<FunctionSig>& overloads) const {
        for (const auto& sig : overloads) {
            if (args_match_sig(args, sig)) {
                return &sig;
            }
        }
        return nullptr;
    }

    [[nodiscard]] bool can_explicit_struct_cast(const SemanticType& from, const SemanticType& to) const {
        if (from.is_error || to.is_error || from.is_pointer || from.is_array) {
            return false;
        }
        const auto it = structs_.find(from.name);
        if (it == structs_.end()) {
            return false;
        }
        for (const auto& conv : it->second.conversions) {
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
            for (const auto& item : block->statements) {
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
                error(s->location, "Incompatible types for return: expected: '{}', got: '{}'", type_to_string(expected), type_to_string(got));
            }
            return;
        }

        if (const auto* s = dynamic_cast<const YieldStmt*>(stmt)) {
            SemanticType got = s->value ? infer_expr_type(s->value.get()) : SemanticType::void_type();
            if (!allow_yield) {
                error(s->location, "Yield allowed only in if/match expressions");
            }
            if (yielded != nullptr) {
                yielded->push_back(got);
            }
            return;
        }

        if (dynamic_cast<const FallthroughStmt*>(stmt)) {
            return;
        }

        if (const auto* s = dynamic_cast<const VarDeclStmt*>(stmt)) {
            SemanticType var_type = from_typeref(s->type);
            var_type.is_array = s->is_array;
            if (!is_known_type(var_type) || (!is_builtin_type_name(var_type.name) && !is_visible_symbol(var_type.name))) {
                error(s->location, "Unknown type '{}' for variable with name '{}'", s->type.name, s->name);
            }

            if (s->init) {
                SemanticType init_type = infer_expr_type(s->init.get());
                SemanticType assign_to = var_type;
                if (assign_to.is_array) {
                    assign_to.is_array = false;
                    assign_to.is_pointer = true;
                }
                if (!is_assignable_to(init_type, assign_to)) {
                    error(s->location,
                          "Incompatible initialization: {} <== {}", type_to_string(assign_to), type_to_string(init_type));
                }
            }
            for (const auto& e : s->array_init) {
                SemanticType item = infer_expr_type(e.get());
                SemanticType elem = var_type;
                elem.is_array = false;
                if (!is_assignable_to(item, elem)) {
                    error(s->location,
                          "Incompatible array element type: {} <== {}", type_to_string(elem), type_to_string(item));
                }
            }

            declare_var(s->name, var_type);
            return;
        }

        if (const auto* s = dynamic_cast<const IfStmt*>(stmt)) {
            SemanticType cond = infer_expr_type(s->condition.get());
            if (!is_bool_like(cond)) {
                error(s->location, "Invalid if expression: expected bool-compatible expression");
            }
            check_statement(s->then_stmt.get(), allow_yield, yielded);
            check_statement(s->else_stmt.get(), allow_yield, yielded);
            return;
        }

        if (const auto* s = dynamic_cast<const WhileStmt*>(stmt)) {
            SemanticType cond = infer_expr_type(s->condition.get());
            if (!is_bool_like(cond)) {
                error(s->location, "Invalid while expression: expected bool-compatible");
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
                if (!(range_t.is_array || range_t.is_pointer)) {
                    warning(s->location, "for-in expects array on right side");
                }
            } else {
                check_statement(s->init.get(), false, nullptr);
                SemanticType cond = s->condition ? infer_expr_type(s->condition.get()) : SemanticType {"bool"};
                if (!is_bool_like(cond)) {
                    error(s->location, "Invalid for expression: expected bool-compatible expression");
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
            error(block->location, "In expression block expects yield expression");
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
            error(block->location,
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
                t.is_pointer = true;
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
            if (current_struct_.has_value()) {
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
            error(e->location, "Unknown identifier: {}", e->name);
            return SemanticType::error();
        }

        if (const auto* e = dynamic_cast<const UnaryExpr*>(expr)) {
            SemanticType operand = infer_expr_type(e->operand.get());
            if (e->op == "..." ) {
                return operand;
            }
            if (e->op == "++" || e->op == "--") {
                if (!is_numeric_type(operand)) {
                    error(e->location, "Operator {} requires numeric operand", e->op);
                    return SemanticType::error();
                }
                return operand;
            }
            if (e->op == "+" || e->op == "-") {
                if (!is_numeric_type(operand)) {
                    error(e->location, "Operator {} requires numeric operand", e->op);
                    return SemanticType::error();
                }
                return operand;
            }
            if (e->op == "!") {
                if (!is_bool_like(operand)) {
                    error(e->location, "Operator {} requires bool-compatible operand", e->op);
                    return SemanticType::error();
                }
                return SemanticType {"bool"};
            }
            if (e->op == "~") {
                if (!is_integer_type(operand)) {
                    error(e->location, "Operator {} requires integer operand", e->op);
                    return SemanticType::error();
                }
                return operand;
            }
            if (e->op == "*") {
                if (!operand.is_pointer && !operand.is_array) {
                    error(e->location, "Operator {} requires array/pointer", e->op);
                    return SemanticType::error();
                }
                operand.is_array = false;
                operand.is_pointer = false;
                return operand;
            }
            if (e->op == "&") {
                if (operand.is_error) {
                    return operand;
                }
                operand.is_pointer = true;
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
                if (!is_assignable_to(rhs, lhs)) {
                    error(e->location,
                          "Incompatible assignment: {} <== {}", type_to_string(lhs), type_to_string(rhs));
                    return SemanticType::error();
                }
                return lhs;
            }

            if (e->op == "+" || e->op == "-" || e->op == "*" || e->op == "/" || e->op == "%") {
                if (!is_numeric_type(lhs) || !is_numeric_type(rhs)) {
                    error(e->location, "Arithmetic operator requires numeric operands");
                    return SemanticType::error();
                }
                return numeric_common_type(lhs, rhs);
            }

            if (e->op == "<" || e->op == ">" || e->op == "<=" || e->op == ">=") {
                if (!((is_numeric_type(lhs) && is_numeric_type(rhs)) || same_type(lhs, rhs))) {
                    error(e->location, "Comparison operator requires compatible types");
                    return SemanticType::error();
                }
                return SemanticType {"bool"};
            }

            if (e->op == "==" || e->op == "!=") {
                if (!((is_numeric_type(lhs) && is_numeric_type(rhs)) || same_type(lhs, rhs))) {
                    error(e->location, "Equality operator requires compatible types");
                    return SemanticType::error();
                }
                return SemanticType {"bool"};
            }

            if (e->op == "&&" || e->op == "||") {
                if (!is_bool_like(lhs) || !is_bool_like(rhs)) {
                    error(e->location, "Logical operator requires bool-compatible operands");
                    return SemanticType::error();
                }
                return SemanticType {"bool"};
            }

            if (e->op == "&" || e->op == "|" || e->op == "^") {
                if (!is_integer_type(lhs) || !is_integer_type(rhs)) {
                    error(e->location, "Byte-each operator requires integer operands");
                    return SemanticType::error();
                }
                return numeric_common_type(lhs, rhs);
            }

            if (e->op == "<<" || e->op == ">>") {
                if (!is_integer_type(lhs) || !is_integer_type(rhs)) {
                    error(e->location, "Shift operator requires integer types");
                    return SemanticType::error();
                }
                return lhs;
            }

            return SemanticType::error();
        }

        if (const auto* e = dynamic_cast<const TernaryExpr*>(expr)) {
            SemanticType cond = infer_expr_type(e->condition.get());
            if (!is_bool_like(cond)) {
                error(e->location, "Ternary condition should be bool-compatible");
            }
            SemanticType a = infer_expr_type(e->then_expr.get());
            SemanticType b = infer_expr_type(e->else_expr.get());
            if (same_type(a, b)) {
                return a;
            }
            if (is_numeric_type(a) && is_numeric_type(b)) {
                return numeric_common_type(a, b);
            }
            error(e->location, "Ternary nodes contains incompatible types: '{}' and '{}'", type_to_string(a), type_to_string(b));
            return SemanticType::error();
        }

        if (const auto* e = dynamic_cast<const MemberExpr*>(expr)) {
            SemanticType obj = infer_expr_type(e->object.get());
            if (obj.is_error) {
                return obj;
            }

            SemanticType base = obj;
            if (e->via_arrow) {
                if (!base.is_pointer) {
                    error(e->location, "Access operator '->' requires pointer type");
                    return SemanticType::error();
                }
                base.is_pointer = false;
            }
            if (base.is_array) {
                base.is_array = false;
            }

            const auto it = structs_.find(base.name);
            if (it == structs_.end()) {
                error(e->location, "<?> Not found structure with name '{}'", base.name);
                return SemanticType::error();
            }

            const auto field = it->second.fields.find(e->member);
            if (field != it->second.fields.end()) {
                return field->second.type;
            }

            const auto method = it->second.methods.find(e->member);
            if (method != it->second.methods.end() && !method->second.empty()) {
                warning(e->location, "Maybe you want to call it?");
                return method->second.front().return_type;
            }

            error(e->location, "In structure: '{}': unknown member with name '{}'", it->second.name, e->member);
            return SemanticType::error();
        }

        if (const auto* e = dynamic_cast<const IndexExpr*>(expr)) {
            SemanticType obj = infer_expr_type(e->object.get());
            SemanticType idx = infer_expr_type(e->index.get());
            if (!is_integer_type(idx)) {
                error(e->location, "Index should be integer");
            }
            if (!(obj.is_array || obj.is_pointer)) {
                error(e->location, "Indexation requires compatible type");
                return SemanticType::error();
            }
            obj.is_array = false;
            obj.is_pointer = false;
            return obj;
        }

        if (const auto* e = dynamic_cast<const TypeCastExpr*>(expr)) {
            SemanticType from = infer_expr_type(e->value.get());
            SemanticType to = from_typeref(e->target_type);
            if (!is_known_type(to) || (!is_builtin_type_name(to.name) && !active_template_types_.contains(to.name) &&
                                       !is_visible_symbol(to.name))) {
                error(e->location, "Unknown target type '{}' in type_cast", e->target_type.name);
                return SemanticType::error();
            }
            if (can_explicit_builtin_cast(from, to) || can_explicit_struct_cast(from, to)) {
                return to;
            }
            error(e->location, "type_cast from {} to {} is not allowed", type_to_string(from), type_to_string(to));
            return SemanticType::error();
        }

        if (const auto* e = dynamic_cast<const CallExpr*>(expr)) {
            std::vector<SemanticType> args;
            args.reserve(e->args.size());
            for (const auto& a : e->args) {
                args.push_back(infer_expr_type(a.get()));
            }

            if (const auto* callee_id = dynamic_cast<const IdentifierExpr*>(e->callee.get())) {
                if (const auto* fns = visible_functions(callee_id->name)) {
                    if (const FunctionSig* sig = choose_overload(args, *fns)) {
                        return sig->return_type;
                    }
                    error(e->location, "Not found compatible overload for function with name '{}'", callee_id->name);
                    return SemanticType::error();
                }
                if (structs_.contains(callee_id->name) && is_visible_symbol(callee_id->name)) {
                    const auto& ctors = structs_[callee_id->name].constructors;
                    if (!ctors.empty() && choose_overload(args, ctors) == nullptr) {
                        error(e->location, "Not found compatible constructor for structure '{}'" + callee_id->name);
                        return SemanticType::error();
                    }
                    SemanticType t;
                    t.name = callee_id->name;
                    return t;
                }
                error(e->location, "Call to undefined function/constructor: {}", callee_id->name);
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
                            }
                            error(member->location, "<?> Not found compatible overload for function from module '{}'", member->member);
                            return SemanticType::error();
                        }
                        error(member->location, "File '{}' does not export symbol with name '{}'", mod->name, member->member);
                        return SemanticType::error();
                    }
                }

                SemanticType obj = infer_expr_type(member->object.get());
                if (obj.is_error) {
                    return obj;
                }
                SemanticType base = obj;
                if (member->via_arrow) {
                    if (!base.is_pointer) {
                        error(member->location, "Operator '->' requires pointer type");
                        return SemanticType::error();
                    }
                    base.is_pointer = false;
                }
                if (base.is_array) {
                    base.is_array = false;
                }

                const auto it = structs_.find(base.name);
                if (it == structs_.end()) {
                    error(member->location, "Method calling available only for structures");
                    return SemanticType::error();
                }
                const auto mit = it->second.methods.find(member->member);
                if (mit == it->second.methods.end()) {
                    error(member->location, "Not found methods with name '{}' in structure with name '{}'", member->member, it->second.name);
                    return SemanticType::error();
                }
                if (const FunctionSig* sig = choose_overload(args, mit->second)) {
                    return sig->return_type;
                }
                error(member->location, "Not found compatible method overload with name '{}'", member->member);
                return SemanticType::error();
            }

            SemanticType callee_t = infer_expr_type(e->callee.get());
            (void)callee_t;
            error(e->location, "Call supported only for named methods/constructors");
            return SemanticType::error();
        }

        if (const auto* e = dynamic_cast<const IfExpr*>(expr)) {
            SemanticType cond = infer_expr_type(e->condition.get());
            if (!is_bool_like(cond)) {
                error(e->location, "If expression condition should be bool-compatible");
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
            error(e->location,
                  "If expression nodes contains incompatible types: {} and {}", type_to_string(then_t), type_to_string(else_t));
            return SemanticType::error();
        }

        if (const auto* e = dynamic_cast<const MatchExpr*>(expr)) {
            SemanticType subj = infer_expr_type(e->subject.get());
            std::vector<SemanticType> result_types;

            for (const auto& c : e->cases) {
                if (!c.is_default && c.match_expr) {
                    SemanticType case_t = infer_expr_type(c.match_expr.get());
                    if (!((is_numeric_type(case_t) && is_numeric_type(subj)) || same_type(case_t, subj))) {
                        error(c.location, "Case condition type '{}' is not compatible with match condition type '{}'", type_to_string(case_t), type_to_string(subj));
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
                error(e->location, "Match expression does not contains returning cases");
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
                error(e->location, "Match cases contains incompatible types");
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

    std::unordered_map<std::string, StructInfo> structs_;
    std::unordered_map<std::string, std::vector<FunctionSig>> functions_;
};

} // namespace

TypeCheckResult type_check(const std::unordered_map<std::string, std::unique_ptr<TranslationUnit>>& units) {
    TypeChecker checker(units);
    return checker.run();
}

} // namespace dino::frontend
