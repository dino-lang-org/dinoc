#pragma once

#include <iosfwd>

#include "dino/cli/link_options.hpp"

namespace dino::cli {

	int run_link(const LinkOptions& options, std::ostream& out, std::ostream& err);

} // namespace dino::cli
