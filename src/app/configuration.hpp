#pragma once

#include <cstdlib>
#include <string>

namespace core {

struct Configuration {
	// spdlog level name: trace | debug | info | warn | error | critical | off.
	std::string log_level = "info";

	// Log file path; logs always go to the console and, when this is non-empty,
	// also to this file. Empty disables file logging.
	std::string log_file = "exchange_tool.log";

	// Build from environment variables, falling back to the defaults above.
	static Configuration from_env() {
		Configuration cfg;
		if (const char *level = std::getenv("LOG_LEVEL")) cfg.log_level = level;
		if (const char *file = std::getenv("LOG_FILE")) cfg.log_file = file;
		return cfg;
	}
};

} // namespace core
