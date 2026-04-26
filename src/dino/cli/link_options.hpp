#pragma once

#include <string>
#include <vector>

namespace dino::cli {

	struct LinkOptions {
		std::vector<std::string> object_files;
		std::string output_file;
		std::vector<std::string> library_paths;
		std::vector<std::string> link_libraries;

		bool is_static_library = false;
	};

} // namespace dino::cli
