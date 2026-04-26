#include "dino/cli/compile.hpp"

#include <fstream>
#include <optional>
#include <sstream>
#include <string>

#include "dino/codegen/backend.hpp"
#include "dino/frontend/dump.hpp"
#include "dino/frontend/lexer.hpp"
#include "dino/frontend/parser.hpp"
#include "dino/frontend/sema.hpp"
#include "dino/frontend/target.hpp"

namespace dino::cli {
	namespace {

		std::string read_file(const std::string& path) {
			std::ifstream in(path, std::ios::binary);
			if (!in) {
				return {};
			}
			std::ostringstream ss;
			ss << in.rdbuf();
			return ss.str();
		}

		void write_text(const std::optional<std::string>& path, const std::string& text, std::ostream& fallback) {
			if (!path.has_value()) {
				fallback << text;
				return;
			}
			std::ofstream out(*path, std::ios::binary);
			out << text;
		}

	} // namespace

	int run_compile(const CompileOptions& options, std::ostream& out, std::ostream& err) {
		frontend::TargetInfo target = frontend::detect_host_target();

		if (!options.target_os.empty()) {
			if (!frontend::is_supported_target_os(options.target_os)) {
				err << "Unsupported target os: " << options.target_os << "\n";
				return 2;
			}
			target.os = options.target_os;
		}
		if (!options.target_arch.empty()) {
			if (!frontend::is_supported_target_arch(options.target_arch)) {
				err << "Unsupported target arch: " << options.target_arch << "\n";
				return 2;
			}
			target.arch = options.target_arch;
		}
		if (!options.target_build_type.empty()) {
			if (!frontend::is_supported_target_build_type(options.target_build_type)) {
				err << "Unsupported target build type: " << options.target_build_type << "\n";
				return 2;
			}
			target.build_type = options.target_build_type;
		}

		frontend::ParserDriver driver(target);
		for (const auto& include_path : options.include_paths) {
			driver.add_include_path(include_path);
		}
		frontend::ParseResult result = driver.parse_entry(options.source_file);
		frontend::TypeCheckResult type_result;
		if (result.ok()) {
			type_result = frontend::type_check(result.units);
		}

		for (const auto& e: result.errors) {
			err << e.location.file << ":" << e.location.line << ":" << e.location.column << ": error: " << e.text << "\n";
		}
		for (const auto& w: result.warnings) {
			err << w.location.file << ":" << w.location.line << ":" << w.location.column << ": warning: " << w.text << "\n";
		}
		for (const auto& e: type_result.errors) {
			err << e.location.file << ":" << e.location.line << ":" << e.location.column << ": error: " << e.text << "\n";
		}
		for (const auto& w: type_result.warnings) {
			err << w.location.file << ":" << w.location.line << ":" << w.location.column << ": warning: " << w.text << "\n";
		}

		if (options.dump_tokens) {
			std::ostringstream token_ss;
			for (const auto& [path, unit]: result.units) {
				const std::string src = read_file(path);
				frontend::Lexer lexer(path, src);
				auto tokens = lexer.tokenize();
				frontend::dump_tokens(tokens, token_ss);
				token_ss << "\n";
			}
			std::string file = options.token_output_file.empty() ? std::string() : options.token_output_file;
			write_text(file.empty() ? std::nullopt : std::optional<std::string>(file), token_ss.str(), out);
		}

		if (options.dump_ast) {
			std::vector<const frontend::TranslationUnit*> units;
			units.reserve(result.units.size());
			for (const auto& [_, unit]: result.units) {
				units.push_back(unit.get());
			}
			std::ostringstream ast_ss;
			frontend::dump_all_asts(units, ast_ss);
			std::string file = options.ast_output_file.empty() ? std::string() : options.ast_output_file;
			write_text(file.empty() ? std::nullopt : std::optional<std::string>(file), ast_ss.str(), out);
		}

		if (!(result.ok() && type_result.ok())) {
			return 1;
		}

		codegen::BackendOptions backend_options;
		backend_options.is_library = true;

		if (!options.llvm_output_file.empty()) {
			backend_options.llvm_output_file = options.llvm_output_file;
		}

		backend_options.object_output_file = options.output_file;

		codegen::LLVMBackend backend(backend_options);
		if (!backend.generate(result.units, err)) {
			return 1;
		}
		if (backend_options.llvm_output_file.has_value() && !backend.write_ir_to_file(*backend_options.llvm_output_file, err)) {
			return 1;
		}
		if (options.dump_llvm_ir && !backend.write_ir(out, err)) {
			return 1;
		}
		if (backend_options.object_output_file.has_value() && !backend.write_object(*backend_options.object_output_file, err)) {
			return 1;
		}

		return 0;
	}

} // namespace dino::cli
