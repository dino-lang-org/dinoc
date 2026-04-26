#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace dino::frontend {

	enum class OutputMode {
		Executable,
		Object,
		LLVMIR,
		SharedLibrary,
		StaticLibrary,
	};

	struct FrontendOptions {
		std::string entry_file;
		bool dump_tokens = false;
		bool dump_ast = false;
		bool dump_llvm_ir = false;
		OutputMode output_mode = OutputMode::Executable;
		std::optional<std::string> token_output_file;
		std::optional<std::string> ast_output_file;
		std::optional<std::string> output_file;
		std::optional<std::string> target_os;
		std::optional<std::string> target_arch;
		std::optional<std::string> target_build_type;
		std::vector<std::string> library_paths;
		std::vector<std::string> link_libraries;
	};

	int run_frontend(const FrontendOptions& options, std::ostream& out, std::ostream& err);

} // namespace dino::frontend
