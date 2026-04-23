#ifdef _WIN32
#include <Windows.h>
#endif

#include "dino/frontend/driver.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    dino::frontend::FrontendOptions options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--dump-tokens") {
            options.dump_tokens = true;
            continue;
        }
        if (arg == "--dump-ast") {
            options.dump_ast = true;
            continue;
        }
        if (arg == "--tokens-out" && i + 1 < argc) {
            options.token_output_file = std::string(argv[++i]);
            continue;
        }
        if (arg == "--ast-out" && i + 1 < argc) {
            options.ast_output_file = std::string(argv[++i]);
            continue;
        }
        if (arg == "--emit-llvm") {
            options.emit_llvm = true;
            continue;
        }
        if (arg == "--llvm-out" && i + 1 < argc) {
            options.emit_llvm = true;
            options.llvm_output_file = std::string(argv[++i]);
            continue;
        }
        if ((arg == "-o" || arg == "--object-out") && i + 1 < argc) {
            options.object_output_file = std::string(argv[++i]);
            continue;
        }
        if (arg == "-h" || arg == "--help") {
            std::cout << "dinoc <entry.dino> [--dump-tokens] [--tokens-out <file>] [--dump-ast] [--ast-out <file>] "
                         "[--emit-llvm] [--llvm-out <file>] [-o <file>]\n";
            return 0;
        }
        if (!arg.empty() && arg[0] != '-') {
            options.entry_file = arg;
            continue;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        return 2;
    }

    if (options.entry_file.empty()) {
        std::cerr << "Entry file is required. Example: dinoc examples/main.dino --emit-llvm --llvm-out out.ll\n";
        return 2;
    }

    return dino::frontend::run_frontend(options, std::cout, std::cerr);
}
