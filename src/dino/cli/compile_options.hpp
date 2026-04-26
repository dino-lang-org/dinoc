#pragma once

#include <string>
#include <vector>

namespace dino::cli {

	struct CompileOptions {
		std::string source_file;
		std::string output_file;
		std::vector<std::string> include_paths;

		std::string target_os;
		std::string target_arch;
		std::string target_build_type;

		bool dump_tokens = false;
		bool dump_ast = false;
		bool dump_llvm_ir = false;
		std::string token_output_file;
		std::string ast_output_file;
		std::string llvm_output_file;
	};

} // namespace dino::cli
