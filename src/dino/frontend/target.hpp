#pragma once

#include <string>

namespace dino::frontend {

	struct TargetInfo {
		std::string os;
		std::string arch;
		std::string build_type;
	};

	TargetInfo detect_host_target();
	bool is_supported_target_os(const std::string& os);
	bool is_supported_target_arch(const std::string& arch);
	bool is_supported_target_build_type(const std::string& build_type);

} // namespace dino::frontend
