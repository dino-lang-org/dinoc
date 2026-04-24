#include "dino/frontend/target.hpp"

namespace dino::frontend {

	bool is_supported_target_os(const std::string& os) {
		return os == "windows" || os == "linux" || os == "macos" || os == "android";
	}

	bool is_supported_target_arch(const std::string& arch) {
		return arch == "x86" || arch == "x86_64" || arch == "arm" || arch == "arm64";
	}

	bool is_supported_target_build_type(const std::string& build_type) {
		return build_type == "debug" || build_type == "release";
	}

	TargetInfo detect_host_target() {
		TargetInfo target;

#if defined(_WIN32)
		target.os = "windows";
#elif defined(__ANDROID__)
		target.os = "android";
#elif defined(__APPLE__)
		target.os = "macos";
#elif defined(__linux__)
		target.os = "linux";
#else
		target.os = "unknown";
#endif

#if defined(_M_X64) || defined(__x86_64__)
		target.arch = "x86_64";
#elif defined(_M_IX86) || defined(__i386__)
		target.arch = "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
		target.arch = "arm64";
#elif defined(_M_ARM) || defined(__arm__)
		target.arch = "arm";
#else
		target.arch = "unknown";
#endif

#ifdef NDEBUG
		target.build_type = "release";
#else
		target.build_type = "debug";
#endif

		return target;
	}

} // namespace dino::frontend
