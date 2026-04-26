#pragma once

#include <iosfwd>

#include "dino/cli/compile_options.hpp"

namespace dino::cli {

	int run_compile(const CompileOptions& options, std::ostream& out, std::ostream& err);

} // namespace dino::cli
