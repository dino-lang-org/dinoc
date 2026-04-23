#pragma once

#include <iosfwd>
#include <optional>
#include <string>

namespace dino::frontend {

struct FrontendOptions {
    std::string entry_file;
    bool dump_tokens = false;
    bool dump_ast = false;
    bool emit_llvm = false;
    std::optional<std::string> token_output_file;
    std::optional<std::string> ast_output_file;
    std::optional<std::string> llvm_output_file;
    std::optional<std::string> object_output_file;
};

int run_frontend(const FrontendOptions& options, std::ostream& out, std::ostream& err);

} // namespace dino::frontend
