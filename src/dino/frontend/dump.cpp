#include "dino/frontend/dump.hpp"

#include <iostream>
#include <variant>

namespace dino::frontend {
namespace {

void indent(std::ostream& os, int level) {
    for (int i = 0; i < level; ++i) {
        os << "  ";
    }
}

void dump_expr(const Expr* expr, std::ostream& os, int level);
void dump_stmt(const Stmt* stmt, std::ostream& os, int level);

void dump_block(const BlockStmt* block, std::ostream& os, int level) {
    indent(os, level);
    os << "Block\n";
    for (const auto& st : block->statements) {
        dump_stmt(st.get(), os, level + 1);
    }
}

void dump_if_expr_branch(const std::variant<ExprPtr, std::unique_ptr<BlockStmt>>& v, std::ostream& os, int level) {
    if (const auto* e = std::get_if<ExprPtr>(&v)) {
        dump_expr(e->get(), os, level);
    } else {
        dump_block(std::get<std::unique_ptr<BlockStmt>>(v).get(), os, level);
    }
}

void dump_expr(const Expr* expr, std::ostream& os, int level) {
    if (!expr) {
        indent(os, level);
        os << "<null-expr>\n";
        return;
    }
    indent(os, level);
    os << expr->kind();

    if (const auto* e = dynamic_cast<const IdentifierExpr*>(expr)) {
        os << " name=" << e->name << "\n";
        return;
    }
    if (const auto* e = dynamic_cast<const LiteralExpr*>(expr)) {
        os << " value=" << e->value << "\n";
        return;
    }
    if (const auto* e = dynamic_cast<const UnaryExpr*>(expr)) {
        os << " op=" << e->op << (e->postfix ? " postfix" : "") << "\n";
        dump_expr(e->operand.get(), os, level + 1);
        return;
    }
    if (const auto* e = dynamic_cast<const BinaryExpr*>(expr)) {
        os << " op=" << e->op << "\n";
        dump_expr(e->lhs.get(), os, level + 1);
        dump_expr(e->rhs.get(), os, level + 1);
        return;
    }
    if (const auto* e = dynamic_cast<const TernaryExpr*>(expr)) {
        os << "\n";
        dump_expr(e->condition.get(), os, level + 1);
        dump_expr(e->then_expr.get(), os, level + 1);
        dump_expr(e->else_expr.get(), os, level + 1);
        return;
    }
    if (const auto* e = dynamic_cast<const CallExpr*>(expr)) {
        os << "\n";
        dump_expr(e->callee.get(), os, level + 1);
        for (const auto& arg : e->args) {
            dump_expr(arg.get(), os, level + 1);
        }
        return;
    }
    if (const auto* e = dynamic_cast<const MemberExpr*>(expr)) {
        os << " member=" << e->member << (e->via_arrow ? " via->" : " via.") << "\n";
        dump_expr(e->object.get(), os, level + 1);
        return;
    }
    if (const auto* e = dynamic_cast<const IndexExpr*>(expr)) {
        os << "\n";
        dump_expr(e->object.get(), os, level + 1);
        dump_expr(e->index.get(), os, level + 1);
        return;
    }
    if (const auto* e = dynamic_cast<const TypeCastExpr*>(expr)) {
        os << " target=" << describe_type(e->target_type) << "\n";
        dump_expr(e->value.get(), os, level + 1);
        return;
    }
    if (const auto* e = dynamic_cast<const IfExpr*>(expr)) {
        os << "\n";
        dump_expr(e->condition.get(), os, level + 1);
        dump_if_expr_branch(e->then_branch, os, level + 1);
        if (e->else_branch.has_value()) {
            dump_if_expr_branch(*e->else_branch, os, level + 1);
        }
        return;
    }
    if (const auto* e = dynamic_cast<const MatchExpr*>(expr)) {
        os << "\n";
        dump_expr(e->subject.get(), os, level + 1);
        for (const auto& c : e->cases) {
            indent(os, level + 1);
            os << (c.is_default ? "DefaultCase" : "Case") << (c.fallthrough ? " fallthrough" : "") << "\n";
            if (c.match_expr) {
                dump_expr(c.match_expr.get(), os, level + 2);
            }
            if (const auto* ce = std::get_if<ExprPtr>(&c.body)) {
                dump_expr(ce->get(), os, level + 2);
            } else {
                dump_block(std::get<std::unique_ptr<BlockStmt>>(c.body).get(), os, level + 2);
            }
        }
        return;
    }

    os << "\n";
}

void dump_stmt(const Stmt* stmt, std::ostream& os, int level) {
    if (!stmt) {
        indent(os, level);
        os << "<null-stmt>\n";
        return;
    }

    indent(os, level);
    os << stmt->kind();
    if (const auto* s = dynamic_cast<const ExprStmt*>(stmt)) {
        os << "\n";
        dump_expr(s->expr.get(), os, level + 1);
        return;
    }
    if (const auto* s = dynamic_cast<const ReturnStmt*>(stmt)) {
        os << "\n";
        dump_expr(s->value.get(), os, level + 1);
        return;
    }
    if (const auto* s = dynamic_cast<const YieldStmt*>(stmt)) {
        os << "\n";
        dump_expr(s->value.get(), os, level + 1);
        return;
    }
    if (dynamic_cast<const FallthroughStmt*>(stmt)) {
        os << "\n";
        return;
    }
    if (const auto* s = dynamic_cast<const VarDeclStmt*>(stmt)) {
        os << " name=" << s->name << " type=" << describe_type(s->type) << "\n";
        if (s->init) {
            dump_expr(s->init.get(), os, level + 1);
        }
        for (const auto& e : s->array_init) {
            dump_expr(e.get(), os, level + 1);
        }
        return;
    }
    if (const auto* s = dynamic_cast<const BlockStmt*>(stmt)) {
        os << "\n";
        for (const auto& item : s->statements) {
            dump_stmt(item.get(), os, level + 1);
        }
        return;
    }
    if (const auto* s = dynamic_cast<const IfStmt*>(stmt)) {
        os << "\n";
        dump_expr(s->condition.get(), os, level + 1);
        dump_stmt(s->then_stmt.get(), os, level + 1);
        dump_stmt(s->else_stmt.get(), os, level + 1);
        return;
    }
    if (const auto* s = dynamic_cast<const WhileStmt*>(stmt)) {
        os << "\n";
        dump_expr(s->condition.get(), os, level + 1);
        dump_stmt(s->body.get(), os, level + 1);
        return;
    }
    if (const auto* s = dynamic_cast<const ForStmt*>(stmt)) {
        os << "\n";
        if (s->range_var.has_value()) {
            indent(os, level + 1);
            os << "RangeVar " << s->range_var->name << " : " << describe_type(s->range_var->type) << "\n";
            dump_expr(s->range_expr.get(), os, level + 1);
        } else {
            dump_stmt(s->init.get(), os, level + 1);
            dump_expr(s->condition.get(), os, level + 1);
            dump_expr(s->step.get(), os, level + 1);
        }
        dump_stmt(s->body.get(), os, level + 1);
        return;
    }

    os << "\n";
}

} // namespace

std::string describe_type(const TypeRef& type) {
    std::string out;
    if (type.is_const) {
        out += "const ";
    }
    out += type.name;
    if (type.is_pointer) {
        out += "*";
    }
    if (type.is_reference) {
        out += "&";
    }
    if (type.variadic) {
        out += "...";
    }
    return out;
}

void dump_tokens(const std::vector<Token>& tokens, std::ostream& os) {
    for (const auto& t : tokens) {
        os << t.location.file << ":" << t.location.line << ":" << t.location.column << " " << to_string(t.type);
        if (!t.lexeme.empty()) {
            os << " '" << t.lexeme << "'";
        }
        os << "\n";
    }
}

void dump_ast(const TranslationUnit& unit, std::ostream& os) {
    os << "TranslationUnit " << unit.file_path << "\n";
    for (const auto& decl : unit.declarations) {
        os << "  " << decl->kind() << " access=" << to_string(decl->access);
        if (const auto* inc = dynamic_cast<const IncludeDecl*>(decl.get())) {
            os << " include=\"" << inc->include_path << "\" resolved=\"" << inc->resolved_path << "\"\n";
            continue;
        }
        if (const auto* fn = dynamic_cast<const FunctionDecl*>(decl.get())) {
            os << " name=" << fn->name << " returns=" << describe_type(fn->return_type) << "\n";
            dump_stmt(fn->body.get(), os, 2);
            continue;
        }
        if (const auto* st = dynamic_cast<const StructDecl*>(decl.get())) {
            os << " name=" << st->name << "\n";
            for (const auto& f : st->fields) {
                os << "    Field access=" << to_string(f.access) << " type=" << describe_type(f.type) << " names=";
                for (const auto& n : f.names) {
                    os << n << " ";
                }
                os << "\n";
            }
            for (const auto& c : st->constructors) {
                os << "    Ctor access=" << to_string(c.access) << " name=" << c.name << "\n";
                dump_stmt(c.body.get(), os, 3);
            }
            for (const auto& d : st->destructors) {
                os << "    Dtor access=" << to_string(d.access) << " name=" << d.name << "\n";
                dump_stmt(d.body.get(), os, 3);
            }
            for (const auto& m : st->methods) {
                os << "    Method access=" << to_string(m.access) << " name=" << m.name << " returns=" << describe_type(m.return_type)
                   << "\n";
                dump_stmt(m.body.get(), os, 3);
            }
            for (const auto& c : st->conversions) {
                os << "    Conversion access=" << to_string(c.access) << " target=" << describe_type(c.target_type) << "\n";
                dump_stmt(c.body.get(), os, 3);
            }
            continue;
        }
        os << "\n";
    }
    os << "  Exported:";
    for (const auto& s : unit.exported_symbols) {
        os << " " << s;
    }
    os << "\n";
    os << "  Local:";
    for (const auto& s : unit.local_symbols) {
        os << " " << s;
    }
    os << "\n";
}

void dump_all_asts(const std::vector<const TranslationUnit*>& units, std::ostream& os) {
    for (const TranslationUnit* unit : units) {
        dump_ast(*unit, os);
        os << "\n";
    }
}

} // namespace dino::frontend
