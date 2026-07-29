#include "replay.hpp"
#include <fstream>
namespace exchange::transport::replay {

std::vector<std::string> read_lines(const char *path) {
	std::ifstream in(path, std::ios::binary);
	std::vector<std::string> lines;
	std::string line;
	while (std::getline(in, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (!line.empty()) lines.push_back(std::move(line));
		line.clear();
	}
	return lines;
}
} // namespace exchange::transport::replay