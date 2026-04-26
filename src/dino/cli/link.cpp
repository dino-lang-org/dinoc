#include "dino/cli/link.hpp"

#include <ostream>

#include "dino/cli/linker.hpp"

namespace dino::cli {

	int run_link(const LinkOptions& options, std::ostream& out, std::ostream& err) {
		if (options.is_static_library) {
			err << "Static library creation is not yet implemented\n";
			return 1;
		}

		LinkerArgs linker_args;
		linker_args.flavor = detect_host_linker();
		linker_args.object_files = options.object_files;
		linker_args.output_file = options.output_file;
		linker_args.library_paths = options.library_paths;
		linker_args.link_libraries = options.link_libraries;
		linker_args.is_dynamic_library = options.is_dynamic_library;
		linker_args.is_static_library = options.is_static_library;

		if (!invoke_linker(linker_args, out, err)) {
			return 1;
		}

		return 0;
	}

} // namespace dino::cli
