#include "dino/frontend/driver.hpp"

#include "dino/codegen/backend.hpp"
#include "dino/frontend/dump.hpp"
#include "dino/frontend/lexer.hpp"
#include "dino/frontend/sema.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace dino::frontend {
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

int run_frontend(const FrontendOptions& options, std::ostream& out, std::ostream& err) {
    ParserDriver driver;
    ParseResult result = driver.parse_entry(options.entry_file);
    TypeCheckResult type_result;
    if (result.ok()) {
        type_result = type_check(result.units);
    }

    for (const auto& e : result.errors) {
        err << e.location.file << ":" << e.location.line << ":" << e.location.column << ": error: " << e.text << "\n";
    }
    for (const auto& w : result.warnings) {
        err << w.location.file << ":" << w.location.line << ":" << w.location.column << ": warning: " << w.text << "\n";
    }
    for (const auto& e : type_result.errors) {
        err << e.location.file << ":" << e.location.line << ":" << e.location.column << ": error: " << e.text << "\n";
    }
    for (const auto& w : type_result.warnings) {
        err << w.location.file << ":" << w.location.line << ":" << w.location.column << ": warning: " << w.text << "\n";
    }

    if (options.dump_tokens) {
        std::ostringstream token_ss;
        for (const auto& [path, unit] : result.units) {
            const std::string src = read_file(path);
            Lexer lexer(path, src);
            auto tokens = lexer.tokenize();
            dump_tokens(tokens, token_ss);
            token_ss << "\n";
        }
        write_text(options.token_output_file, token_ss.str(), out);
    }

    if (options.dump_ast) {
        std::vector<const TranslationUnit*> units;
        units.reserve(result.units.size());
        for (const auto& [_, unit] : result.units) {
            units.push_back(unit.get());
        }
        std::ostringstream ast_ss;
        dump_all_asts(units, ast_ss);
        write_text(options.ast_output_file, ast_ss.str(), out);
    }

    if (!(result.ok() && type_result.ok())) {
        return 1;
    }

    codegen::BackendOptions backend_options;
    backend_options.emit_llvm = options.emit_llvm || options.llvm_output_file.has_value();
    backend_options.llvm_output_file = options.llvm_output_file;
    backend_options.object_output_file = options.object_output_file;

    codegen::LLVMBackend backend(backend_options);
    if (!backend.generate(result.units, err)) {
        return 1;
    }
    if (backend_options.emit_llvm && !backend.write_ir(out, err)) {
        return 1;
    }
    if (backend_options.object_output_file.has_value() && !backend.write_object(*backend_options.object_output_file, err)) {
        return 1;
    }

    return 0;
}

} // namespace dino::frontend
