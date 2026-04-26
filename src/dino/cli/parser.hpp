#pragma once

#include <optional>
#include <string>
#include <variant>

#include "dino/cli/compile_options.hpp"
#include "dino/cli/link_options.hpp"

namespace dino::cli {

	enum class CommandType {
		None,
		Help,
		Compile,
		Link,
	};

	struct ParsedCommand {
		CommandType type = CommandType::None;
		std::variant<std::monostate, CompileOptions, LinkOptions> options;
		std::string error_message;
	};

	ParsedCommand parse_command_line(int argc, char** argv);

} // namespace dino::cli
