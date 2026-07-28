#include "configuration.hpp"

#include <cstdlib>

namespace exchange::app {

Configuration Configuration::from_env() {
	Configuration cfg;
	if (const char *level = std::getenv("LOG_LEVEL")) cfg.log_level = level;
	if (const char *file = std::getenv("LOG_FILE")) cfg.log_file = file;
	return cfg;
}

} // namespace exchange::app
