#include "slurp.hpp"

#include <fstream>
#include <sstream>

namespace exchange::core::util{
std::string slurp(const char *path) {
	std::ifstream in(path, std::ios::binary);
	std::ostringstream ss;
	ss << in.rdbuf();
	return ss.str();
}
} // namespace exchange::util