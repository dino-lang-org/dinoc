#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace dino::cli {

	enum class LinkerFlavor {
		COFF,    // Windows (lld-link)
		ELF,     // Linux (ld.lld)
		MachO,   // macOS (ld64.lld)
	};

	struct LinkerArgs {
		LinkerFlavor flavor;
		std::vector<std::string> object_files;
		std::string output_file;
		std::vector<std::string> library_paths;
		std::vector<std::string> link_libraries;
		bool is_dynamic_library = false;
		bool is_static_library = false;
	};

	LinkerFlavor detect_host_linker();
	bool invoke_linker(const LinkerArgs& args, std::ostream& out, std::ostream& err);

} // namespace dino::cli
