#include "dino/cli/linker.hpp"

#include <ostream>
#include <vector>

#include "lld/Common/Driver.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/raw_ostream.h"

LLD_HAS_DRIVER(coff)
LLD_HAS_DRIVER(elf)
LLD_HAS_DRIVER(macho)

namespace dino::cli {

	LinkerFlavor detect_host_linker() {
#ifdef _WIN32
		return LinkerFlavor::COFF;
#elif defined(__APPLE__)
		return LinkerFlavor::MachO;
#else
		return LinkerFlavor::ELF;
#endif
	}

	namespace {

		void build_coff_args(const LinkerArgs& args, std::vector<std::string>& storage, std::vector<const char*>& result) {
			storage.push_back("lld-link");

			if (args.is_dynamic_library) {
				storage.push_back("/DLL");
			} else {
				storage.push_back("/SUBSYSTEM:CONSOLE");
			}

			storage.push_back("/OUT:" + args.output_file);

			for (const auto& obj : args.object_files) {
				storage.push_back(obj);
			}

			for (const auto& lib_path : args.library_paths) {
				storage.push_back("/LIBPATH:" + lib_path);
			}

			for (const auto& lib : args.link_libraries) {
				storage.push_back(lib + ".lib");
			}

			storage.push_back("libcmt.lib");
			storage.push_back("libvcruntime.lib");
			storage.push_back("libucrt.lib");
			storage.push_back("kernel32.lib");

			storage.push_back("/NOLOGO");
			storage.push_back("/MACHINE:X64");
			storage.push_back("/NODEFAULTLIB");

			for (const auto& str : storage) {
				result.push_back(str.c_str());
			}
		}

		void build_elf_args(const LinkerArgs& args, std::vector<std::string>& storage, std::vector<const char*>& result) {
			storage.push_back("ld.lld");

			if (args.is_dynamic_library) {
				storage.push_back("-shared");
			}

			storage.push_back("-o");
			storage.push_back(args.output_file);

			for (const auto& obj : args.object_files) {
				storage.push_back(obj);
			}

			for (const auto& lib_path : args.library_paths) {
				storage.push_back("-L");
				storage.push_back(lib_path);
			}

			for (const auto& lib : args.link_libraries) {
				storage.push_back("-l" + lib);
			}

			for (const auto& str : storage) {
				result.push_back(str.c_str());
			}
		}

		void build_macho_args(const LinkerArgs& args, std::vector<std::string>& storage, std::vector<const char*>& result) {
			storage.push_back("ld64.lld");

			if (args.is_dynamic_library) {
				storage.push_back("-dylib");
			}

			storage.push_back("-o");
			storage.push_back(args.output_file);

			for (const auto& obj : args.object_files) {
				storage.push_back(obj);
			}

			for (const auto& lib_path : args.library_paths) {
				storage.push_back("-L");
				storage.push_back(lib_path);
			}

			for (const auto& lib : args.link_libraries) {
				storage.push_back("-l" + lib);
			}

			for (const auto& str : storage) {
				result.push_back(str.c_str());
			}
		}

	} // namespace

	bool invoke_linker(const LinkerArgs& args, std::ostream& out, std::ostream& err) {
		std::vector<std::string> arg_storage;
		std::vector<const char*> lld_args;

		switch (args.flavor) {
		case LinkerFlavor::COFF:
			build_coff_args(args, arg_storage, lld_args);
			break;
		case LinkerFlavor::ELF:
			build_elf_args(args, arg_storage, lld_args);
			break;
		case LinkerFlavor::MachO:
			build_macho_args(args, arg_storage, lld_args);
			break;
		}

		llvm::raw_fd_ostream llvm_out(1, false);
		llvm::raw_fd_ostream llvm_err(2, false);

		lld::DriverDef drivers[] = {
			{lld::WinLink, &lld::coff::link},
			{lld::Gnu, &lld::elf::link},
			{lld::Darwin, &lld::macho::link},
		};

		lld::Result result = lld::lldMain(llvm::ArrayRef<const char*>(lld_args.data(), lld_args.size()), llvm_out, llvm_err, llvm::ArrayRef<lld::DriverDef>(drivers, 3));

		return result.retCode == 0;
	}

} // namespace dino::cli
