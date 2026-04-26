#ifdef _WIN32
#include <Windows.h>
#endif

#include <iostream>
#include <variant>

#include "dino/cli/compile.hpp"
#include "dino/cli/link.hpp"
#include "dino/cli/parser.hpp"

int main(int argc, char** argv) {
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
#endif

	dino::cli::ParsedCommand cmd = dino::cli::parse_command_line(argc, argv);

	if (!cmd.error_message.empty()) {
		std::cerr << "Error: " << cmd.error_message << "\n";
		return 2;
	}

	if (cmd.type == dino::cli::CommandType::Help) {
		return 0;
	}

	if (cmd.type == dino::cli::CommandType::Compile) {
		const auto& options = std::get<dino::cli::CompileOptions>(cmd.options);
		return dino::cli::run_compile(options, std::cout, std::cerr);
	}

	if (cmd.type == dino::cli::CommandType::Link) {
		const auto& options = std::get<dino::cli::LinkOptions>(cmd.options);
		return dino::cli::run_link(options, std::cout, std::cerr);
	}

	std::cerr << "No command specified. Use --help for usage information.\n";
	return 2;
}
