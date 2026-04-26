#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "dino/frontend/ast.hpp"
#include "dino/frontend/target.hpp"
#include "dino/frontend/token.hpp"

namespace dino::frontend {

	struct ParseOptions {
		bool dump_tokens = false;
		bool dump_ast = false;
	};

	struct ParseMessage {
		SourceLocation location;
		std::string text;
	};

	struct ParseResult {
		std::unordered_map<std::string, std::unique_ptr<TranslationUnit>> units;
		std::vector<ParseMessage> errors;
		std::vector<ParseMessage> warnings;

		bool ok() const { return errors.empty(); }
	};

	class ParserDriver {
	public:
		explicit ParserDriver(TargetInfo target = detect_host_target())
			: target_(std::move(target)) {}

		void add_include_path(const std::string& path) { include_paths_.push_back(path); }

		ParseResult parse_entry(const std::string& entry_path);

	private:
		bool parse_unit_recursive(const std::string& path, ParseResult& result);
		std::string resolve_include_path(const std::string& include_path, const std::string& current_file) const;

		TargetInfo target_;
		std::vector<std::string> include_paths_;
	};

	std::string resolve_include_path(const std::string& include_path, const std::string& current_file);

} // namespace dino::frontend
