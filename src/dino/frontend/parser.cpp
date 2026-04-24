
#include "dino/frontend/parser.hpp"

#include "dino/frontend/lexer.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace dino::frontend {
namespace fs = std::filesystem;

namespace {

bool is_builtin_type(TokenType t) {
    switch (t) {
    case TokenType::KwInt8:
    case TokenType::KwInt16:
    case TokenType::KwInt32:
    case TokenType::KwInt64:
    case TokenType::KwUint8:
    case TokenType::KwUint16:
    case TokenType::KwUint32:
    case TokenType::KwUint64:
    case TokenType::KwFloat:
    case TokenType::KwDouble:
    case TokenType::KwChar:
    case TokenType::KwVoid:
    case TokenType::KwBool:
        return true;
    default:
        return false;
    }
}

class Parser {
public:
    Parser(std::vector<Token> tokens, std::vector<ParseMessage>& errors)
        : tokens_(std::move(tokens)), errors_(errors) {}

    std::unique_ptr<TranslationUnit> parse_translation_unit(const std::string& file_path) {
        auto unit = std::make_unique<TranslationUnit>();
        unit->file_path = file_path;
        while (!check(TokenType::EndOfFile)) {
            if (auto decl = parse_declaration()) {
                if (auto name = decl->declared_name(); name.has_value()) {
                    unit->local_symbols.insert(*name);
                    if (decl->access == AccessModifier::Public) {
                        unit->exported_symbols.insert(*name);
                    }
                }
                unit->declarations.push_back(std::move(decl));
            } else {
                synchronize_top_level();
            }
        }
        return unit;
    }

private:
    FunctionAttributes parse_function_attributes() {
        FunctionAttributes attributes;
        while (match(TokenType::Hash)) {
            expect(TokenType::LBracket, "Expected '[' after '#'");
            auto name = expect(TokenType::Identifier, "Expected attribute name inside '#[...]'");
            expect(TokenType::RBracket, "Expected ']' after attribute name");
            if (!name) {
                continue;
            }
            if (name->lexeme == "extern") {
                attributes.is_extern = true;
            } else if (name->lexeme == "no_mangle") {
                attributes.no_mangle = true;
            } else {
                error(*name, "Unknown attribute '{}'", name->lexeme);
            }
        }
        return attributes;
    }

    std::unique_ptr<BlockStmt> parse_optional_function_body() {
        if (check(TokenType::LBrace)) {
            return parse_block_stmt();
        }
        if (match(TokenType::Semicolon)) {
            return nullptr;
        }
        error(current(), "Expected function body or ';'");
        return nullptr;
    }

    DeclPtr parse_declaration() {
        FunctionAttributes attributes = parse_function_attributes();
        std::vector<std::string> template_params;
        while (match(TokenType::KwTemplate)) {
            auto parsed = parse_template_params();
            template_params.insert(template_params.end(), parsed.begin(), parsed.end());
        }

        AccessModifier access = AccessModifier::Private;
        if (match(TokenType::KwPublic)) {
            access = AccessModifier::Public;
        } else if (match(TokenType::KwPrivate)) {
            access = AccessModifier::Private;
        }

        if (check(TokenType::At)) {
            if (attributes.uses_c_abi()) {
                error(current(), "Attributes '#[extern]' and '#[no_mangle]' are only allowed on functions and methods");
            }
            auto include = parse_include_decl(access);
            if (include) {
                include->template_params = std::move(template_params);
            }
            return include;
        }

        if (match(TokenType::KwStruct)) {
            if (attributes.uses_c_abi()) {
                error(current(), "Attributes '#[extern]' and '#[no_mangle]' are only allowed on functions and methods");
            }
            auto decl = parse_struct_decl(access);
            if (decl) {
                decl->template_params = std::move(template_params);
            }
            return decl;
        }

        if (looks_like_function_decl()) {
            auto decl = parse_function_decl(access, attributes);
            if (decl) {
                decl->template_params = std::move(template_params);
            }
            return decl;
        }

        error(current(), "Expected top-level declaration");
        return nullptr;
    }

    std::vector<std::string> parse_template_params() {
        std::vector<std::string> out;
        expect(TokenType::Less, "Expected '<' after template");
        while (!check(TokenType::Greater) && !check(TokenType::EndOfFile)) {
            if (check(TokenType::KwTypename) || (check(TokenType::Identifier) && current().lexeme == "class")) {
                advance();
            }
            if (match(TokenType::Ellipsis)) {}
            if (check(TokenType::Identifier)) {
                out.push_back(advance().lexeme);
            } else {
                advance();
            }
            if (!match(TokenType::Comma)) {}
        }
        expect(TokenType::Greater, "Expected '>' after template parameters ");
        return out;
    }

    std::unique_ptr<IncludeDecl> parse_include_decl(AccessModifier access) {
        auto at = expect(TokenType::At, "Expected '@'");
        // TODO: Change the error message when adding other directives
        if (!expect(TokenType::KwInclude, "Expected 'include' after '@'")) {
            return nullptr;
        }
        expect(TokenType::LParen, "Expected '(' after include");
        auto path_tok = expect(TokenType::String, "Expected path in the include");
        expect(TokenType::RParen, "Expected ')' after include");
        match(TokenType::Semicolon);

        if (!path_tok) {
            return nullptr;
        }

        auto decl = std::make_unique<IncludeDecl>();
        decl->location = at ? at->location : path_tok->location;
        decl->access = access;
        std::string path = path_tok->lexeme;
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
            path = path.substr(1, path.size() - 2);
        }
        decl->include_path = std::move(path);
        return decl;
    }

    std::unique_ptr<StructDecl> parse_struct_decl(AccessModifier access) {
        auto name = expect(TokenType::Identifier, "Expected structure name");
        if (!name) {
            return nullptr;
        }

        auto decl = std::make_unique<StructDecl>();
        decl->location = name->location;
        decl->access = access;
        decl->name = name->lexeme;

        expect(TokenType::LBrace, "Expected '{' after field declaration");

        while (!check(TokenType::RBrace) && !check(TokenType::EndOfFile)) {
            FunctionAttributes member_attributes = parse_function_attributes();
            AccessModifier member_access = AccessModifier::Private;
            if (match(TokenType::KwPublic)) {
                member_access = AccessModifier::Public;
            } else if (match(TokenType::KwPrivate)) {
                member_access = AccessModifier::Private;
            }
            const bool member_static = match(TokenType::KwStatic);

            if (check(TokenType::Tilde)) {
                if (member_static) {
                    error(current(), "Keyword 'static' is not allowed on destructors");
                }
                if (member_attributes.uses_c_abi()) {
                    error(current(), "Attributes '#[extern]' and '#[no_mangle]' are only allowed on methods");
                }
                if (auto destructor = parse_destructor_decl(member_access, decl->name); destructor) {
                    decl->destructors.push_back(std::move(*destructor));
                }
                continue;
            }

            if (check(TokenType::Identifier) && current().lexeme == decl->name && peek().type == TokenType::LParen) {
                if (member_static) {
                    error(current(), "Keyword 'static' is not allowed on constructors");
                }
                if (member_attributes.uses_c_abi()) {
                    error(current(), "Attributes '#[extern]' and '#[no_mangle]' are only allowed on methods");
                }
                if (auto constructor = parse_constructor_decl(member_access, decl->name); constructor) {
                    decl->constructors.push_back(std::move(*constructor));
                }
                continue;
            }

            if (looks_like_method_or_field()) {
                TypeRef type = parse_type_ref();
                if (check(TokenType::LParen)) {
                    if (member_static) {
                        error(current(), "Keyword 'static' is not allowed on conversion operators");
                    }
                    if (member_attributes.uses_c_abi()) {
                        error(current(), "Attributes '#[extern]' and '#[no_mangle]' are only allowed on methods");
                    }
                    ConversionDecl conv;
                    conv.location = current().location;
                    conv.access = member_access;
                    conv.target_type = std::move(type);
                    expect(TokenType::LParen, "Expected '(' after transformator type declaration");
                    expect(TokenType::RParen, "Expected ')' after transformator type declaration");
                    conv.body = parse_block_stmt();
                    if (!conv.body) {
                        synchronize_struct();
                        continue;
                    }
                    decl->conversions.push_back(std::move(conv));
                    continue;
                }

                auto id = expect(TokenType::Identifier, "Expected field/method name");
                if (!id) {
                    synchronize_struct();
                    continue;
                }

                if (check(TokenType::LParen)) {
                    MethodDecl m;
                    m.location = id->location;
                    m.access = member_access;
                    m.attributes = member_attributes;
                    m.is_static = member_static;
                    m.return_type = std::move(type);
                    m.name = id->lexeme;
                    m.parameters = parse_parameter_list();
                    m.body = parse_optional_function_body();
                    if (m.body) {
                        inject_nonull_param_checks(m.body, m.parameters);
                    }
                    decl->methods.push_back(std::move(m));
                } else {
                    if (member_attributes.uses_c_abi()) {
                        error(*id, "Attributes '#[extern]' and '#[no_mangle]' are only allowed on methods");
                    }
                    FieldDecl f;
                    f.location = id->location;
                    f.access = member_access;
                    f.is_static = member_static;
                    f.type = std::move(type);
                    f.names.push_back(id->lexeme);
                    while (match(TokenType::Comma)) {
                        auto n = expect(TokenType::Identifier, "Expected field name");
                        if (!n) {
                            break;
                        }
                        f.names.push_back(n->lexeme);
                    }
                    expect(TokenType::Semicolon, "Expected ';' after field/method");
                    decl->fields.push_back(std::move(f));
                }
                continue;
            }

            error(current(), "Expected field, method, constructor or destructor");
            synchronize_struct();
        }

        expect(TokenType::RBrace, "Expected '}' after structure body");
        return decl;
    }

    std::optional<ConstructorDecl> parse_constructor_decl(AccessModifier access, const std::string& name) {
        ConstructorDecl ctor;
        ctor.access = access;
        auto n = expect(TokenType::Identifier, "Expected constructor name");
        if (!n) {
            return std::nullopt;
        }
        ctor.location = n->location;
        ctor.name = n->lexeme;
        ctor.parameters = parse_parameter_list();
        if (match(TokenType::Colon)) {
            while (!check(TokenType::LBrace) && !check(TokenType::Semicolon) && !check(TokenType::EndOfFile)) {
                advance();
            }
        }
        ctor.body = parse_block_stmt();
        if (!ctor.body) {
            return std::nullopt;
        }
        inject_nonull_param_checks(ctor.body, ctor.parameters);
        if (ctor.name != name) {
            error(*n, "Constructor name should be same with the structure name");
        }
        return ctor;
    }

    std::optional<DestructorDecl> parse_destructor_decl(AccessModifier access, const std::string& name) {
        DestructorDecl dtor;
        dtor.access = access;
        auto tilde = expect(TokenType::Tilde, "Expected '~' before destructor declaration");
        auto n = expect(TokenType::Identifier, "Expected destructor name");
        if (!n) {
            return std::nullopt;
        }
        dtor.location = tilde ? tilde->location : n->location;
        dtor.name = n->lexeme;
        expect(TokenType::LParen, "Expected '(");
        expect(TokenType::RParen, "Expected ')");
        dtor.body = parse_block_stmt();
        if (!dtor.body) {
            return std::nullopt;
        }
        if (dtor.name != name) {
            error(*n, "Destructor name should be same with the structure name");
        }
        return dtor;
    }

    std::unique_ptr<FunctionDecl> parse_function_decl(AccessModifier access, const FunctionAttributes& attributes) {
        TypeRef ret = parse_type_ref();
        auto name = expect(TokenType::Identifier, "Expected function name");
        if (!name) {
            return nullptr;
        }

        auto decl = std::make_unique<FunctionDecl>();
        decl->access = access;
        decl->attributes = attributes;
        decl->location = name->location;
        decl->return_type = std::move(ret);
        decl->name = name->lexeme;
        decl->parameters = parse_parameter_list();
        decl->body = parse_optional_function_body();
        if (decl->body) {
            inject_nonull_param_checks(decl->body, decl->parameters);
        }
        return decl;
    }

    std::vector<Parameter> parse_parameter_list() {
        std::vector<Parameter> params;
        expect(TokenType::LParen, "Expected '('");
        while (!check(TokenType::RParen) && !check(TokenType::EndOfFile)) {
            Parameter p;
            if (match(TokenType::Ellipsis)) {
                p.type.name = "void";
                p.type.variadic = true;
                params.push_back(std::move(p));
                break;
            }
            p.type = parse_type_ref();
            if (match(TokenType::Ellipsis)) {
                p.type.variadic = true;
            }
            if (check(TokenType::Identifier)) {
                p.name = advance().lexeme;
            }
            params.push_back(std::move(p));
            if (!match(TokenType::Comma)) {
                break;
            }
        }
        expect(TokenType::RParen, "Expected ')' after parameters");
        return params;
    }

    TypeRef parse_type_ref() {
        TypeRef type;

        while (check(TokenType::Identifier) && (current().lexeme == "const" || current().lexeme == "nonull")) {
            if (current().lexeme == "const") {
                type.is_const = true;
            } else if (current().lexeme == "nonull") {
                type.is_nonull = true;
            }
            advance();
        }

        if (is_builtin_type(current().type) || current().type == TokenType::Identifier) {
            type.name = advance().lexeme;
        } else {
            error(current(), "Expected type name");
            type.name = "<error>";
            return type;
        }

        while (true) {
            if (match(TokenType::Star)) {
                type.is_pointer = true;
                continue;
            }
            if (match(TokenType::And)) {
                type.is_reference = true;
                continue;
            }
            if (check(TokenType::Identifier) && (current().lexeme == "const" || current().lexeme == "nonull")) {
                if (current().lexeme == "const") {
                    type.is_const = true;
                } else if (current().lexeme == "nonull") {
                    type.is_nonull = true;
                }
                advance();
                continue;
            }
            break;
        }

        return type;
    }

    StmtPtr make_nonull_check(const std::string& var_name, const SourceLocation& loc) {
        auto cond_expr = std::make_unique<UnaryExpr>();
        cond_expr->location = loc;
        cond_expr->op = "!";
        auto id_expr = std::make_unique<IdentifierExpr>();
        id_expr->location = loc;
        id_expr->name = var_name;
        cond_expr->operand = std::move(id_expr);

        auto panic_call = std::make_unique<CallExpr>();
        panic_call->location = loc;
        auto panic_id = std::make_unique<IdentifierExpr>();
        panic_id->location = loc;
        panic_id->name = "panic";
        panic_call->callee = std::move(panic_id);
        auto msg = std::make_unique<LiteralExpr>();
        msg->location = loc;
        msg->value = "\"" + var_name + " should be valid\"";
        msg->literal_kind = "String";
        panic_call->args.push_back(std::move(msg));

        auto expr_stmt = std::make_unique<ExprStmt>();
        expr_stmt->location = loc;
        expr_stmt->expr = std::move(panic_call);

        auto if_stmt = std::make_unique<IfStmt>();
        if_stmt->location = loc;
        if_stmt->condition = std::move(cond_expr);
        if_stmt->then_stmt = std::move(expr_stmt);
        return if_stmt;
    }

    void inject_nonull_param_checks(std::unique_ptr<BlockStmt>& body, const std::vector<Parameter>& params) {
        if (!body) {
            return;
        }
        std::vector<StmtPtr> checks;
        for (const auto& p : params) {
            if (p.type.is_nonull && !p.name.empty()) {
                checks.push_back(make_nonull_check(p.name, body->location));
            }
        }
        if (!checks.empty()) {
            checks.insert(checks.end(), std::make_move_iterator(body->statements.begin()),
                          std::make_move_iterator(body->statements.end()));
            body->statements = std::move(checks);
        }
    }

    std::unique_ptr<BlockStmt> parse_block_stmt() {
        if (!expect(TokenType::LBrace, "Expected '{'")) {
            return nullptr;
        }

        auto block = std::make_unique<BlockStmt>();
        block->location = previous().location;
        while (!check(TokenType::RBrace) && !check(TokenType::EndOfFile)) {
            if (auto st = parse_statement()) {
                if (auto* var_decl = dynamic_cast<VarDeclStmt*>(st.get())) {
                    if (var_decl->needs_nonull_check) {
                        block->statements.push_back(std::move(st));
                        block->statements.push_back(make_nonull_check(var_decl->name, var_decl->location));
                        continue;
                    }
                }
                block->statements.push_back(std::move(st));
            } else {
                synchronize_statement();
            }
        }
        expect(TokenType::RBrace, "Expected '}'");
        return block;
    }

    StmtPtr parse_statement() {
        if (check(TokenType::LBrace)) {
            return parse_block_stmt();
        }
        if (match(TokenType::KwReturn)) {
            auto st = std::make_unique<ReturnStmt>();
            st->location = previous().location;
            if (!check(TokenType::Semicolon)) {
                st->value = parse_expression();
            }
            expect(TokenType::Semicolon, "Expected ';' after return");
            return st;
        }
        if (match(TokenType::KwYield)) {
            auto st = std::make_unique<YieldStmt>();
            st->location = previous().location;
            st->value = parse_expression();
            consume_optional_terminator();
            return st;
        }
        if (match(TokenType::KwFallthrough)) {
            auto st = std::make_unique<FallthroughStmt>();
            st->location = previous().location;
            consume_optional_terminator();
            return st;
        }
        if (match(TokenType::KwDelete)) {
            auto st = std::make_unique<DeleteStmt>();
            st->location = previous().location;
            st->value = parse_expression();
            expect(TokenType::Semicolon, "Expected ';' after delete expression");
            return st;
        }
        if (match(TokenType::KwIf)) {
            return parse_if_stmt(previous().location);
        }
        if (match(TokenType::KwWhile)) {
            return parse_while_stmt(previous().location);
        }
        if (match(TokenType::KwFor)) {
            return parse_for_stmt(previous().location);
        }

        if (looks_like_var_decl_stmt()) {
            return parse_var_decl_stmt();
        }

        auto st = std::make_unique<ExprStmt>();
        st->location = current().location;
        st->expr = parse_expression();
        expect(TokenType::Semicolon, "Expected ';' after expression");
        return st;
    }

    StmtPtr parse_if_stmt(const SourceLocation& if_loc) {
        expect(TokenType::LParen, "Expected '('");
        auto cond = parse_expression();
        expect(TokenType::RParen, "Expected ')'");

        auto st = std::make_unique<IfStmt>();
        st->location = if_loc;
        st->condition = std::move(cond);
        st->then_stmt = parse_statement();
        if (match(TokenType::KwElse)) {
            st->else_stmt = parse_statement();
        }
        return st;
    }

    StmtPtr parse_while_stmt(const SourceLocation& while_loc) {
        expect(TokenType::LParen, "Expected '('");
        auto cond = parse_expression();
        expect(TokenType::RParen, "Expected ')'");

        auto st = std::make_unique<WhileStmt>();
        st->location = while_loc;
        st->condition = std::move(cond);
        st->body = parse_statement();
        return st;
    }

    StmtPtr parse_for_stmt(const SourceLocation& for_loc) {
        expect(TokenType::LParen, "Expected '('");

        auto st = std::make_unique<ForStmt>();
        st->location = for_loc;

        if (looks_like_range_for()) {
            Parameter p;
            p.type = parse_type_ref();
            if (auto n = expect(TokenType::Identifier, "Expected variable name in for-in")) {
                p.name = n->lexeme;
            }
            expect(TokenType::KwIn, "Expected in in for-in");
            st->range_var = std::move(p);
            st->range_expr = parse_expression();
            expect(TokenType::RParen, "Expected ')' after for-in");
            st->body = parse_statement();
            return st;
        }

        if (match(TokenType::Semicolon)) {
        } else if (looks_like_var_decl_stmt()) {
            st->init = parse_var_decl_stmt(false);
        } else {
            auto init = std::make_unique<ExprStmt>();
            init->location = current().location;
            init->expr = parse_expression();
            st->init = std::move(init);
            expect(TokenType::Semicolon, "Expected ';' after init in for");
        }

        if (!check(TokenType::Semicolon)) {
            st->condition = parse_expression();
        }
        expect(TokenType::Semicolon, "Expected ';' after expression in for");

        if (!check(TokenType::RParen)) {
            st->step = parse_expression();
        }
        expect(TokenType::RParen, "Expected ')' after expression in for");
        st->body = parse_statement();
        return st;
    }

    StmtPtr parse_var_decl_stmt(bool semicolon_consumed = false) {
        TypeRef type = parse_type_ref();
        auto name = expect(TokenType::Identifier, "Expected variable name");
        if (!name) {
            return nullptr;
        }

        auto st = std::make_unique<VarDeclStmt>();
        st->location = name->location;
        st->type = std::move(type);
        st->name = name->lexeme;

        if (match(TokenType::LBracket)) {
            expect(TokenType::RBracket, "Expected '[' after array name");
            st->is_array = true;
        }

        if (match(TokenType::Assign)) {
            if (match(TokenType::LBrace)) {
                while (!check(TokenType::RBrace) && !check(TokenType::EndOfFile)) {
                    st->array_init.push_back(parse_expression());
                    if (!match(TokenType::Comma)) {
                        break;
                    }
                }
                expect(TokenType::RBrace, "Expected '}' after array initialization");
            } else {
                st->init = parse_expression();
            }
        }

        if (!semicolon_consumed) {
            if (match(TokenType::Semicolon)) {
                // Don't return here - check for nonull first
            } else {
                const bool is_yielding_expr =
                    st->init && (dynamic_cast<IfExpr*>(st->init.get()) != nullptr || dynamic_cast<MatchExpr*>(st->init.get()) != nullptr);
                const bool safe_boundary =
                    check(TokenType::KwElse) || check(TokenType::KwCase) || check(TokenType::KwDefault) || check(TokenType::RBrace) ||
                    check(TokenType::EndOfFile) || check(TokenType::KwIf) || check(TokenType::KwWhile) || check(TokenType::KwFor) ||
                    check(TokenType::KwReturn) || check(TokenType::KwYield) || check(TokenType::KwFallthrough) || check(TokenType::LBrace) ||
                    is_type_start(current());
                if (!(is_yielding_expr && safe_boundary)) {
                    expect(TokenType::Semicolon, "Expected ';' after variable declaration");
                }
            }
        } else if (!match(TokenType::Semicolon)) {
            if (!(check(TokenType::KwElse) || check(TokenType::KwCase) || check(TokenType::KwDefault) || check(TokenType::RBrace) ||
                  check(TokenType::EndOfFile))) {
                expect(TokenType::Semicolon, "Expeced ';' after variable declaration");
            }
        }

        if (st->type.is_nonull) {
            st->needs_nonull_check = true;
        }

        return st;
    }

    ExprPtr parse_expression() { return parse_assignment(); }

    ExprPtr parse_assignment() {
        auto lhs = parse_ternary();
        if (match(TokenType::Assign) || match(TokenType::PlusAssign) || match(TokenType::MinusAssign) ||
            match(TokenType::StarAssign) || match(TokenType::SlashAssign) || match(TokenType::PercentAssign) ||
            match(TokenType::AndAssign) || match(TokenType::OrAssign) || match(TokenType::XorAssign) ||
            match(TokenType::ShiftLeftAssign) || match(TokenType::ShiftRightAssign)) {
            const Token op = previous();
            auto rhs = parse_assignment();
            auto bin = std::make_unique<BinaryExpr>();
            bin->location = op.location;
            bin->op = op.lexeme;
            bin->lhs = std::move(lhs);
            bin->rhs = std::move(rhs);
            return bin;
        }
        return lhs;
    }

    ExprPtr parse_ternary() {
        auto cond = parse_logical_or();
        if (match(TokenType::Question)) {
            error(previous(), "Ternary operator is not supported; use 'if (cond) yield ... else yield ...' instead");
            auto then_e = parse_expression();
            (void)then_e;
            expect(TokenType::Colon, "Expected ':' in ternary expression");
            auto else_e = parse_expression();
            (void)else_e;
            auto id = std::make_unique<IdentifierExpr>();
            id->location = cond ? cond->location : previous().location;
            id->name = "<error>";
            return id;
        }
        return cond;
    }

    ExprPtr parse_logical_or() { return parse_left_assoc(&Parser::parse_logical_and, {TokenType::OrOr}); }
    ExprPtr parse_logical_and() { return parse_left_assoc(&Parser::parse_bitwise_or, {TokenType::AndAnd}); }
    ExprPtr parse_bitwise_or() { return parse_left_assoc(&Parser::parse_bitwise_xor, {TokenType::Or}); }
    ExprPtr parse_bitwise_xor() { return parse_left_assoc(&Parser::parse_bitwise_and, {TokenType::Xor}); }
    ExprPtr parse_bitwise_and() { return parse_left_assoc(&Parser::parse_equality, {TokenType::And}); }
    ExprPtr parse_equality() { return parse_left_assoc(&Parser::parse_relational, {TokenType::Equal, TokenType::NotEqual}); }
    ExprPtr parse_relational() {
        return parse_left_assoc(
            &Parser::parse_shift,
            {TokenType::Less, TokenType::Greater, TokenType::LessEqual, TokenType::GreaterEqual});
    }
    ExprPtr parse_shift() { return parse_left_assoc(&Parser::parse_additive, {TokenType::ShiftLeft, TokenType::ShiftRight}); }
    ExprPtr parse_additive() { return parse_left_assoc(&Parser::parse_multiplicative, {TokenType::Plus, TokenType::Minus}); }
    ExprPtr parse_multiplicative() { return parse_left_assoc(&Parser::parse_unary, {TokenType::Star, TokenType::Slash, TokenType::Percent}); }

    ExprPtr parse_left_assoc(ExprPtr (Parser::*subparser)(), std::initializer_list<TokenType> ops) {
        auto lhs = (this->*subparser)();
        while (match_any(ops)) {
            const Token op = previous();
            auto rhs = (this->*subparser)();
            auto bin = std::make_unique<BinaryExpr>();
            bin->location = op.location;
            bin->op = op.lexeme;
            bin->lhs = std::move(lhs);
            bin->rhs = std::move(rhs);
            lhs = std::move(bin);
        }
        return lhs;
    }

    ExprPtr parse_unary() {
        if (match(TokenType::PlusPlus) || match(TokenType::MinusMinus) || match(TokenType::Plus) ||
            match(TokenType::Minus) || match(TokenType::Not) || match(TokenType::Tilde) || match(TokenType::Star) ||
            match(TokenType::And)) {
            const Token op = previous();
            auto expr = std::make_unique<UnaryExpr>();
            expr->location = op.location;
            expr->op = op.lexeme;
            expr->operand = parse_unary();
            return expr;
        }
        return parse_postfix();
    }

    ExprPtr parse_postfix() {
        auto expr = parse_primary();
        while (true) {
            if (match(TokenType::LParen)) {
                auto call = std::make_unique<CallExpr>();
                call->location = previous().location;
                call->callee = std::move(expr);
                while (!check(TokenType::RParen) && !check(TokenType::EndOfFile)) {
                    call->args.push_back(parse_expression());
                    if (!match(TokenType::Comma)) {
                        break;
                    }
                }
                expect(TokenType::RParen, "Expected ')' after arguments");
                expr = std::move(call);
                continue;
            }
            if (match(TokenType::Dot) || match(TokenType::Arrow)) {
                const bool via_arrow = previous().type == TokenType::Arrow;
                if (match(TokenType::Tilde)) {
                    auto type_name = expect(TokenType::Identifier, "Expected type name after '~' in destructor call");
                    if (!type_name) {
                        return expr;
                    }
                    expect(TokenType::LParen, "Expected '(' after destructor name");
                    expect(TokenType::RParen, "Expected ')' after destructor call");
                    auto dtor = std::make_unique<DestructorCallExpr>();
                    dtor->location = type_name->location;
                    dtor->object = std::move(expr);
                    dtor->type_name = type_name->lexeme;
                    dtor->via_arrow = via_arrow;
                    expr = std::move(dtor);
                    continue;
                }
                auto member = expect(TokenType::Identifier, "Expected field/method name");
                if (!member) {
                    return expr;
                }
                auto m = std::make_unique<MemberExpr>();
                m->location = member->location;
                m->object = std::move(expr);
                m->member = member->lexeme;
                m->via_arrow = via_arrow;
                expr = std::move(m);
                continue;
            }
            if (match(TokenType::LBracket)) {
                auto idx = std::make_unique<IndexExpr>();
                idx->location = previous().location;
                idx->object = std::move(expr);
                idx->index = parse_expression();
                expect(TokenType::RBracket, "Expected ']' after index");
                expr = std::move(idx);
                continue;
            }
            if (match(TokenType::PlusPlus) || match(TokenType::MinusMinus)) {
                const Token op = previous();
                auto u = std::make_unique<UnaryExpr>();
                u->location = op.location;
                u->op = op.lexeme;
                u->postfix = true;
                u->operand = std::move(expr);
                expr = std::move(u);
                continue;
            }
            if (match(TokenType::Ellipsis)) {
                auto u = std::make_unique<UnaryExpr>();
                u->location = previous().location;
                u->op = "...";
                u->postfix = true;
                u->operand = std::move(expr);
                expr = std::move(u);
                continue;
            }
            break;
        }
        return expr;
    }

    ExprPtr parse_primary() {
        if (match(TokenType::KwIf)) {
            return parse_if_expression(previous().location);
        }
        if (match(TokenType::KwMatch)) {
            return parse_match_expression(previous().location);
        }
        if (match(TokenType::KwNew)) {
            auto expr = std::make_unique<NewExpr>();
            expr->location = previous().location;
            if (match(TokenType::LParen)) {
                expr->placement = parse_expression();
                expect(TokenType::RParen, "Expected ')' after placement new address");
            }
            expr->target_type = parse_type_ref();
            if (match(TokenType::LBracket)) {
                expr->is_array = true;
                expr->array_size = parse_expression();
                expect(TokenType::RBracket, "Expected ']' after new array size");
            }
            if (match(TokenType::LParen)) {
                while (!check(TokenType::RParen) && !check(TokenType::EndOfFile)) {
                    expr->args.push_back(parse_expression());
                    if (!match(TokenType::Comma)) {
                        break;
                    }
                }
                expect(TokenType::RParen, "Expected ')' after new arguments");
            }
            return expr;
        }
        if (match(TokenType::Number) || match(TokenType::String) || match(TokenType::Character) ||
            match(TokenType::KwTrue) || match(TokenType::KwFalse)) {
            auto lit = std::make_unique<LiteralExpr>();
            lit->location = previous().location;
            lit->value = previous().lexeme;
            lit->literal_kind = to_string(previous().type);
            return lit;
        }
        if (match(TokenType::Identifier) || match(TokenType::KwThis)) {
            if (previous().type == TokenType::Identifier && previous().lexeme == "type_cast" && check(TokenType::Less)) {
                auto type_cast = std::make_unique<TypeCastExpr>();
                type_cast->location = previous().location;
                expect(TokenType::Less, "Expected '<' after type_cast");
                type_cast->target_type = parse_type_ref();
                expect(TokenType::Greater, "Expected '>' after type in type_cast");
                expect(TokenType::LParen, "Expected '(' after type_cast<{}>", type_cast->target_type.name);
                type_cast->value = parse_expression();
                expect(TokenType::RParen, "Expected ')' after type_cast");
                return type_cast;
            }
            auto id = std::make_unique<IdentifierExpr>();
            id->location = previous().location;
            id->name = previous().lexeme;
            return id;
        }
        if (match(TokenType::LParen)) {
            auto expr = parse_expression();
            expect(TokenType::RParen, "Expected ')' after expression");
            return expr;
        }

        error(current(), "Expected expression");
        auto id = std::make_unique<IdentifierExpr>();
        id->location = current().location;
        id->name = "<error>";
        advance();
        return id;
    }

    ExprPtr parse_if_expression(const SourceLocation& loc) {
        expect(TokenType::LParen, "Expected '(' after if");
        auto condition = parse_expression();
        expect(TokenType::RParen, "Expected ')' after if condition");

        auto expr = std::make_unique<IfExpr>();
        expr->location = loc;
        expr->condition = std::move(condition);

        expr->then_branch = parse_if_expr_branch();
        if (match(TokenType::KwElse)) {
            expr->else_branch = parse_if_expr_branch();
        }
        return expr;
    }

    std::variant<ExprPtr, std::unique_ptr<BlockStmt>> parse_if_expr_branch() {
        if (check(TokenType::LBrace)) {
            return parse_block_stmt();
        }
        if (match(TokenType::KwYield)) {
            auto yielded = parse_expression();
            consume_optional_terminator();
            return yielded;
        }
        return parse_expression();
    }

    ExprPtr parse_match_expression(const SourceLocation& loc) {
        expect(TokenType::LParen, "Expected '(' after match");
        auto subject = parse_expression();
        expect(TokenType::RParen, "Expected '(' after match subject");
        expect(TokenType::LBrace, "Expected '{' after match");

        auto m = std::make_unique<MatchExpr>();
        m->location = loc;
        m->subject = std::move(subject);

        while (!check(TokenType::RBrace) && !check(TokenType::EndOfFile)) {
            MatchCase c;
            c.location = current().location;
            if (match(TokenType::KwCase)) {
                c.match_expr = parse_expression();
            } else if (match(TokenType::KwDefault)) {
                c.is_default = true;
            } else {
                error(current(), "Expected at least default case in match");
                break;
            }
            expect(TokenType::Colon, "Expected ':' after match case");

            if (match(TokenType::KwFallthrough)) {
                c.fallthrough = true;
                consume_optional_terminator();
            } else if (check(TokenType::LBrace)) {
                c.body = parse_block_stmt();
            } else if (match(TokenType::KwYield)) {
                c.body = parse_expression();
                consume_optional_terminator();
            } else {
                c.body = parse_expression();
                consume_optional_terminator();
            }

            m->cases.push_back(std::move(c));
        }
        expect(TokenType::RBrace, "Expected '}' after match");
        return m;
    }

    [[nodiscard]] bool looks_like_method_or_field() const { return is_type_start(current()); }

    [[nodiscard]] bool looks_like_function_decl() const {
        if (!is_type_start(current())) {
            return false;
        }
        size_t i = pos_;
        consume_type_preview(i);
        if (at(i).type != TokenType::Identifier) {
            return false;
        }
        ++i;
        return at(i).type == TokenType::LParen;
    }

    [[nodiscard]] bool looks_like_var_decl_stmt() const {
        if (!is_type_start(current())) {
            return false;
        }
        size_t i = pos_;
        consume_type_preview(i);
        if (at(i).type != TokenType::Identifier) {
            return false;
        }
        ++i;
        const TokenType t = at(i).type;
        return t == TokenType::Assign || t == TokenType::Semicolon || t == TokenType::LBracket || t == TokenType::Comma;
    }

    [[nodiscard]] bool looks_like_range_for() const {
        if (!is_type_start(current())) {
            return false;
        }
        size_t i = pos_;
        consume_type_preview(i);
        if (at(i).type != TokenType::Identifier) {
            return false;
        }
        ++i;
        return at(i).type == TokenType::KwIn;
    }

    [[nodiscard]] bool is_type_start(const Token& t) const {
        if (is_builtin_type(t.type)) {
            return true;
        }
        return t.type == TokenType::Identifier;
    }

    void consume_type_preview(size_t& idx) const {
        while (at(idx).type == TokenType::Identifier && (at(idx).lexeme == "const" || at(idx).lexeme == "nonull")) {
            ++idx;
        }
        if (is_builtin_type(at(idx).type) || at(idx).type == TokenType::Identifier) {
            ++idx;
        }
        while (at(idx).type == TokenType::Star || at(idx).type == TokenType::And) {
            ++idx;
        }
    }

    void consume_optional_terminator() {
        if (match(TokenType::Semicolon)) {
            return;
        }
        if (check(TokenType::KwElse) || check(TokenType::KwCase) || check(TokenType::KwDefault) || check(TokenType::RBrace) ||
            check(TokenType::EndOfFile)) {
            return;
        }
    }

    void synchronize_top_level() {
        while (!check(TokenType::EndOfFile)) {
            if (match(TokenType::Semicolon)) {
                return;
            }
            if (check(TokenType::KwPublic) || check(TokenType::KwPrivate) || check(TokenType::KwStruct) ||
                check(TokenType::At) || check(TokenType::KwTemplate) || is_type_start(current())) {
                return;
            }
            advance();
        }
    }

    void synchronize_struct() {
        while (!check(TokenType::EndOfFile)) {
            if (check(TokenType::Semicolon) || check(TokenType::RBrace)) {
                return;
            }
            advance();
        }
    }

    void synchronize_statement() {
        while (!check(TokenType::EndOfFile)) {
            if (match(TokenType::Semicolon)) {
                return;
            }
            if (check(TokenType::RBrace)) {
                return;
            }
            advance();
        }
    }

    [[nodiscard]] const Token& current() const { return tokens_[pos_]; }
    [[nodiscard]] const Token& previous() const { return tokens_[pos_ - 1]; }

    [[nodiscard]] const Token& at(size_t p) const {
        if (p >= tokens_.size()) {
            return tokens_.back();
        }
        return tokens_[p];
    }

    [[nodiscard]] const Token& peek() const { return at(pos_ + 1); }

    const Token& advance() {
        if (!check(TokenType::EndOfFile)) {
            ++pos_;
        }
        return previous();
    }

    [[nodiscard]] bool check(TokenType type) const { return current().type == type; }

    bool match(TokenType type) {
        if (!check(type)) {
            return false;
        }
        advance();
        return true;
    }

    bool match_any(std::initializer_list<TokenType> types) {
        for (TokenType type : types) {
            if (check(type)) {
                advance();
                return true;
            }
        }
        return false;
    }

    const Token* expect(TokenType type, const std::string& format, auto&&... args) {
        if (check(type)) {
            return &advance();
        }
        error(current(), format, args...);
        return nullptr;
    }

    void error(const Token& where, const std::string& format, auto&&... args) {
        errors_.push_back(ParseMessage {where.location, std::vformat(format, std::make_format_args(args...))});
    }

private:
    std::vector<Token> tokens_;
    size_t pos_ = 0;
    std::vector<ParseMessage>& errors_;
};

std::string read_file(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    std::ostringstream ss;
    ss << input.rdbuf();
    return ss.str();
}

} // namespace

std::string resolve_include_path(const std::string& include_path, const std::string& current_file) {
    fs::path include = include_path;
    if (include.extension().empty()) {
        include += ".dino";
    }

    fs::path current = fs::path(current_file).parent_path();
    fs::path candidate = current / include;
    if (fs::exists(candidate)) {
        return fs::weakly_canonical(candidate).string();
    }

    if (fs::exists(include)) {
        return fs::weakly_canonical(include).string();
    }

    return fs::weakly_canonical(candidate).string();
}

ParseResult ParserDriver::parse_entry(const std::string& entry_path) {
    ParseResult result;
    fs::path p = fs::weakly_canonical(fs::path(entry_path));
    parse_unit_recursive(p.string(), result);
    return result;
}

bool ParserDriver::parse_unit_recursive(const std::string& path, ParseResult& result) {
    if (result.units.contains(path)) {
        return true;
    }

    const std::string source = read_file(path);
    if (source.empty()) {
        result.errors.push_back(ParseMessage {SourceLocation {path, 1, 1}, "Can't read file"});
        return false;
    }

    Lexer lexer(path, source);
    auto tokens = lexer.tokenize();

    Parser parser(std::move(tokens), result.errors);
    auto unit = parser.parse_translation_unit(path);

    std::vector<std::pair<IncludeDecl*, std::string>> includes;
    for (auto& decl : unit->declarations) {
        if (auto* include = dynamic_cast<IncludeDecl*>(decl.get())) {
            include->resolved_path = resolve_include_path(include->include_path, path);
            includes.emplace_back(include, include->resolved_path);
        }
    }

    result.units[path] = std::move(unit);

    for (const auto& [_, include_path] : includes) {
        parse_unit_recursive(include_path, result);
    }

    auto* current = result.units[path].get();
    for (const auto& [include_decl, include_path] : includes) {
        const auto found = result.units.find(include_path);
        if (found == result.units.end()) {
            continue;
        }

        for (const auto& symbol : found->second->exported_symbols) {
            current->local_symbols.insert(symbol);
            if (include_decl->access == AccessModifier::Public) {
                current->exported_symbols.insert(symbol);
            }
        }
    }

    return true;
}

} // namespace dino::frontend
