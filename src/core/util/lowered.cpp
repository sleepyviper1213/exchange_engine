#include "lowered.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace exchange::core::util {

std::string lowered(std::string_view text) {
	std::string out(text);
	std::ranges::transform(out, out.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	return out;
}

} // namespace exchange::core::util
