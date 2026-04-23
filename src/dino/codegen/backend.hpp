#pragma once

#include "dino/frontend/ast.hpp"

#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace llvm {
class LLVMContext;
class Module;
}

namespace dino::codegen {

struct BackendOptions {
    bool emit_llvm = false;
    std::optional<std::string> llvm_output_file;
    std::optional<std::string> object_output_file;
};

class LLVMBackend {
public:
    explicit LLVMBackend(BackendOptions options = {});
    ~LLVMBackend();

    bool generate(const std::unordered_map<std::string, std::unique_ptr<frontend::TranslationUnit>>& units, std::ostream& err);
    bool write_ir(std::ostream& out, std::ostream& err) const;
    bool write_object(const std::string& output_path, std::ostream& err) const;

private:
    BackendOptions options_;
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
};

} // namespace dino::codegen
