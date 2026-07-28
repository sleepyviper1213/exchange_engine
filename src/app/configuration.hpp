#pragma once

#include <string>

namespace exchange::app {

struct Configuration {
	// spdlog level name: trace | debug | info | warn | error | critical | off.
	std::string log_level = "info";

	// Log file path; logs always go to the console and, when this is non-empty,
	// also to this file. Empty disables file logging.
	std::string log_file = "exchange_tool.log";

	// Build from environment variables, falling back to the defaults above.
	// Defined in configuration.cpp so <cstdlib> stays out of this header.
	static Configuration from_env();
};

} // namespace exchange::app
