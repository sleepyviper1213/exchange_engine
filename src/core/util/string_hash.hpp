#pragma once
#include <string>

namespace exchange::core::util {
struct string_hash {
	using is_transparent = void;

	size_t operator()(std::string_view sv) const {
		return std::hash<std::string_view>{}(sv);
	}

	size_t operator()(const std::string &str) const {
		return std::hash<std::string>{}(str);
	}
};
} // namespace exchange::core::util