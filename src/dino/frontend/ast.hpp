#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

#include "dino/frontend/token.hpp"

namespace dino::frontend {

	struct Expr;
	struct Stmt;
	struct Decl;
	struct TranslationUnit;

	using ExprPtr = std::unique_ptr<Expr>;
	using StmtPtr = std::unique_ptr<Stmt>;
	using DeclPtr = std::unique_ptr<Decl>;

	struct TemplateParam {
		std::string name;
		bool is_pack = false;
	};

	struct Node {
		SourceLocation location;
		virtual ~Node() = default;
	};

	struct Expr : Node {
		[[nodiscard]] virtual std::string kind() const = 0;
	};

	struct Stmt : Node {
		[[nodiscard]] virtual std::string kind() const = 0;
	};

	struct Decl : Node {
		AccessModifier access = AccessModifier::Private;
		std::vector<TemplateParam> template_params;
		[[nodiscard]] virtual std::string kind() const = 0;
		[[nodiscard]] virtual std::optional<std::string> declared_name() const { return std::nullopt; }
	};

	struct TypeRef {
		std::string name;
		bool is_const = false;
		bool is_nonull = false;
		int pointer_depth = 0;
		bool is_reference = false;
		bool variadic = false;
	};

	struct Parameter {
		TypeRef type;
		std::string name;
		bool is_pack = false;
	};

	struct FunctionAttributes {
		bool is_extern = false;
		bool no_mangle = false;

		[[nodiscard]] bool uses_c_abi() const { return is_extern || no_mangle; }
	};

	struct LiteralExpr : Expr {
		std::string value;
		std::string literal_kind;
		[[nodiscard]] std::string kind() const override { return "LiteralExpr"; }
	};

	struct IdentifierExpr : Expr {
		std::string name;
		std::vector<TypeRef> template_args;
		[[nodiscard]] std::string kind() const override { return "IdentifierExpr"; }
	};

	struct UnaryExpr : Expr {
		std::string op;
		ExprPtr operand;
		bool postfix = false;
		[[nodiscard]] std::string kind() const override { return "UnaryExpr"; }
	};

	struct BinaryExpr : Expr {
		std::string op;
		ExprPtr lhs;
		ExprPtr rhs;
		[[nodiscard]] std::string kind() const override { return "BinaryExpr"; }
	};

	struct CallExpr : Expr {
		ExprPtr callee;
		std::vector<ExprPtr> args;
		[[nodiscard]] std::string kind() const override { return "CallExpr"; }
	};

	struct MemberExpr : Expr {
		ExprPtr object;
		std::string member;
		bool via_arrow = false;
		[[nodiscard]] std::string kind() const override { return "MemberExpr"; }
	};

	struct IndexExpr : Expr {
		ExprPtr object;
		ExprPtr index;
		[[nodiscard]] std::string kind() const override { return "IndexExpr"; }
	};

	struct TypeCastExpr : Expr {
		TypeRef target_type;
		ExprPtr value;
		[[nodiscard]] std::string kind() const override { return "TypeCastExpr"; }
	};

	struct NewExpr : Expr {
		ExprPtr placement;
		TypeRef target_type;
		bool is_array = false;
		ExprPtr array_size;
		std::vector<ExprPtr> args;
		[[nodiscard]] std::string kind() const override { return "NewExpr"; }
	};

	struct DestructorCallExpr : Expr {
		ExprPtr object;
		std::string type_name;
		bool via_arrow = false;
		[[nodiscard]] std::string kind() const override { return "DestructorCallExpr"; }
	};

	struct BlockStmt;

	struct IfExpr : Expr {
		ExprPtr condition;
		std::variant<ExprPtr, std::unique_ptr<BlockStmt>> then_branch;
		std::optional<std::variant<ExprPtr, std::unique_ptr<BlockStmt>>> else_branch;
		[[nodiscard]] std::string kind() const override { return "IfExpr"; }
	};

	struct MatchCase {
		bool is_default = false;
		ExprPtr match_expr;
		std::variant<ExprPtr, std::unique_ptr<BlockStmt>> body;
		bool fallthrough = false;
		SourceLocation location;
	};

	struct MatchExpr : Expr {
		ExprPtr subject;
		std::vector<MatchCase> cases;
		[[nodiscard]] std::string kind() const override { return "MatchExpr"; }
	};

	struct ExprStmt : Stmt {
		ExprPtr expr;
		[[nodiscard]] std::string kind() const override { return "ExprStmt"; }
	};

	struct ReturnStmt : Stmt {
		ExprPtr value;
		[[nodiscard]] std::string kind() const override { return "ReturnStmt"; }
	};

	struct YieldStmt : Stmt {
		ExprPtr value;
		[[nodiscard]] std::string kind() const override { return "YieldStmt"; }
	};

	struct FallthroughStmt : Stmt {
		[[nodiscard]] std::string kind() const override { return "FallthroughStmt"; }
	};

	struct DeleteStmt : Stmt {
		ExprPtr value;
		[[nodiscard]] std::string kind() const override { return "DeleteStmt"; }
	};

	struct VarDeclStmt : Stmt {
		TypeRef type;
		std::string name;
		bool is_static = false;
		bool is_array = false;
		// True when the user wrote `= { ... }` (possibly empty), so the variable
		// has an initializer even if `array_init` is empty. Lets the semantic
		// checker accept `const T x[] = { };` while still rejecting `const T x[];`.
		bool has_brace_init = false;
		ExprPtr init;
		std::vector<ExprPtr> array_init;
		bool needs_nonull_check = false;
		[[nodiscard]] std::string kind() const override { return "VarDeclStmt"; }
	};

	struct BlockStmt : Stmt {
		std::vector<StmtPtr> statements;
		[[nodiscard]] std::string kind() const override { return "BlockStmt"; }
	};

	struct IfStmt : Stmt {
		ExprPtr condition;
		StmtPtr then_stmt;
		StmtPtr else_stmt;
		[[nodiscard]] std::string kind() const override { return "IfStmt"; }
	};

	struct WhileStmt : Stmt {
		ExprPtr condition;
		StmtPtr body;
		[[nodiscard]] std::string kind() const override { return "WhileStmt"; }
	};

	struct ForStmt : Stmt {
		StmtPtr init;
		ExprPtr condition;
		ExprPtr step;
		std::optional<Parameter> range_var;
		ExprPtr range_expr;
		StmtPtr body;
		[[nodiscard]] std::string kind() const override { return "ForStmt"; }
	};

	struct IncludeDecl : Decl {
		std::string include_path;
		std::string resolved_path;
		[[nodiscard]] std::string kind() const override { return "IncludeDecl"; }
	};

	struct FunctionDecl : Decl {
		FunctionAttributes attributes;
		TypeRef return_type;
		std::string name;
		std::vector<Parameter> parameters;
		std::unique_ptr<BlockStmt> body;
		[[nodiscard]] std::string kind() const override { return "FunctionDecl"; }
		[[nodiscard]] std::optional<std::string> declared_name() const override { return name; }
	};

	struct GlobalVarDecl : Decl {
		bool is_extern = false;
		TypeRef type;
		std::string name;
		bool is_array = false;
		// True when the user wrote `= { ... }` (possibly empty); see VarDeclStmt.
		bool has_brace_init = false;
		ExprPtr init;
		std::vector<ExprPtr> array_init;
		[[nodiscard]] std::string kind() const override { return "GlobalVarDecl"; }
		[[nodiscard]] std::optional<std::string> declared_name() const override { return name; }
	};

	struct FieldDecl : Node {
		AccessModifier access = AccessModifier::Private;
		bool is_static = false;
		TypeRef type;
		std::vector<std::string> names;
		ExprPtr init;
	};

	struct ConstructorDecl : Node {
		AccessModifier access = AccessModifier::Private;
		std::string name;
		std::vector<Parameter> parameters;
		std::unique_ptr<BlockStmt> body;
	};

	struct DestructorDecl : Node {
		AccessModifier access = AccessModifier::Private;
		std::string name;
		std::unique_ptr<BlockStmt> body;
	};

	struct MethodDecl : Node {
		AccessModifier access = AccessModifier::Private;
		FunctionAttributes attributes;
		bool is_static = false;
		std::vector<TemplateParam> template_params;
		TypeRef return_type;
		std::string name;
		std::vector<Parameter> parameters;
		std::unique_ptr<BlockStmt> body;
	};

	struct ConversionDecl : Node {
		AccessModifier access = AccessModifier::Private;
		TypeRef target_type;
		std::unique_ptr<BlockStmt> body;
	};

	struct StructDecl : Decl {
		std::string name;
		std::vector<FieldDecl> fields;
		std::vector<ConstructorDecl> constructors;
		std::vector<DestructorDecl> destructors;
		std::vector<MethodDecl> methods;
		std::vector<ConversionDecl> conversions;
		[[nodiscard]] std::string kind() const override { return "StructDecl"; }
		[[nodiscard]] std::optional<std::string> declared_name() const override { return name; }
	};

	struct TranslationUnit {
		std::string file_path;
		std::vector<DeclPtr> declarations;
		std::unordered_set<std::string> exported_symbols;
		std::unordered_set<std::string> local_symbols;
	};

} // namespace dino::frontend
