#include "dino/cli/parser.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace dino::cli {
	namespace {

		void print_help() {
			std::cout << "Usage: dinoc <command> [options]\n\n"
						 "Commands:\n"
						 "  compile <source.dino>  Compile source file to object file\n"
						 "  link <obj1.o> ...      Link object files into executable or library\n"
						 "  --help, -h             Show this help message\n\n"
						 "Compile options:\n"
						 "  -o <file>              Output object file (default: source_name.o)\n"
						 "  -I <path>              Add include search path\n"
						 "  --target-os <os>       Target OS (windows, linux, macos)\n"
						 "  --target-arch <arch>   Target architecture (x86_64, aarch64)\n"
						 "  --target-build-type <type>  Build type (debug, release)\n"
						 "  --dump-tokens          Print tokens to stdout\n"
						 "  --tokens-out <file>    Write tokens to file\n"
						 "  --dump-ast             Print AST to stdout\n"
						 "  --ast-out <file>       Write AST to file\n"
						 "  --dump-llvm            Print LLVM IR to stdout\n"
						 "  --llvm-out <file>      Write LLVM IR to file\n\n"
						 "Link options:\n"
						 "  -o <file>              Output executable/library file\n"
						 "  -L <path>              Add library search path\n"
						 "  -l <lib>               Link with library\n"
						 "  -statlib               Create static library (no entry point check)\n";
		}

		ParsedCommand parse_compile_command(int argc, char** argv, int start_idx) {
			ParsedCommand result;
			result.type = CommandType::Compile;
			CompileOptions options;

			if (start_idx >= argc) {
				result.error_message = "compile: source file is required";
				return result;
			}

			options.source_file = argv[start_idx];
			++start_idx;

			for (int i = start_idx; i < argc; ++i) {
				const std::string arg = argv[i];

				if (arg == "-o" && i + 1 < argc) {
					options.output_file = argv[++i];
				} else if (arg == "-I" && i + 1 < argc) {
					options.include_paths.emplace_back(argv[++i]);
				} else if (arg == "--target-os" && i + 1 < argc) {
					options.target_os = argv[++i];
				} else if (arg == "--target-arch" && i + 1 < argc) {
					options.target_arch = argv[++i];
				} else if (arg == "--target-build-type" && i + 1 < argc) {
					options.target_build_type = argv[++i];
				} else if (arg == "--dump-tokens") {
					options.dump_tokens = true;
				} else if (arg == "--tokens-out" && i + 1 < argc) {
					options.token_output_file = argv[++i];
				} else if (arg == "--dump-ast") {
					options.dump_ast = true;
				} else if (arg == "--ast-out" && i + 1 < argc) {
					options.ast_output_file = argv[++i];
				} else if (arg == "--dump-llvm") {
					options.dump_llvm_ir = true;
				} else if (arg == "--llvm-out" && i + 1 < argc) {
					options.llvm_output_file = argv[++i];
				} else {
					result.error_message = "compile: unknown argument: " + arg;
					return result;
				}
			}

			if (options.output_file.empty()) {
				std::filesystem::path source_path(options.source_file);
				options.output_file = source_path.stem().string() + ".o";
			}

			result.options = options;
			return result;
		}

		ParsedCommand parse_link_command(int argc, char** argv, int start_idx) {
			ParsedCommand result;
			result.type = CommandType::Link;
			LinkOptions options;

			for (int i = start_idx; i < argc; ++i) {
				const std::string arg = argv[i];

				if (arg == "-o" && i + 1 < argc) {
					options.output_file = argv[++i];
				} else if (arg == "-L" && i + 1 < argc) {
					options.library_paths.emplace_back(argv[++i]);
				} else if (arg == "-l" && i + 1 < argc) {
					options.link_libraries.emplace_back(argv[++i]);
				} else if (arg == "-statlib") {
					options.is_static_library = true;
				} else if (!arg.empty() && arg[0] != '-') {
					options.object_files.emplace_back(arg);
				} else {
					result.error_message = "link: unknown argument: " + arg;
					return result;
				}
			}

			if (options.object_files.empty()) {
				result.error_message = "link: at least one object file is required";
				return result;
			}

			if (options.output_file.empty()) {
				options.output_file = options.is_static_library ? "lib.a" : "a.out";
			}

			result.options = options;
			return result;
		}

	} // namespace

	ParsedCommand parse_command_line(int argc, char** argv) {
		ParsedCommand result;

		if (argc < 2) {
			result.error_message = "No command specified. Use --help for usage information.";
			return result;
		}

		const std::string command = argv[1];

		if (command == "-h" || command == "--help") {
			result.type = CommandType::Help;
			print_help();
			return result;
		}

		if (command == "compile") {
			return parse_compile_command(argc, argv, 2);
		}

		if (command == "link") {
			return parse_link_command(argc, argv, 2);
		}

		result.error_message = "Unknown command: " + command + ". Use --help for usage information.";
		return result;
	}

} // namespace dino::cli
