#include "dino/cli/link.hpp"

#include <ostream>

#include "dino/codegen/backend.hpp"

namespace dino::cli {

	int run_link(const LinkOptions& options, std::ostream& out, std::ostream& err) {
		if (options.is_static_library) {
			err << "Static library creation is not yet implemented\n";
			return 1;
		}

		codegen::BackendOptions backend_options;
		backend_options.is_library = false;
		backend_options.library_paths = options.library_paths;
		backend_options.link_libraries = options.link_libraries;
		backend_options.executable_output_file = options.output_file;

		codegen::LLVMBackend backend(backend_options);

		if (options.object_files.size() == 1) {
			if (!backend.link_executable(options.object_files[0], options.output_file, err)) {
				return 1;
			}
		} else {
			err << "Linking multiple object files is not yet fully implemented\n";
			return 1;
		}

		return 0;
	}

} // namespace dino::cli
