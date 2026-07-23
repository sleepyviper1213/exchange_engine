#include "logger.hpp"

#include "configuration.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <vector>

void core::init_logging(const Configuration &config) {
	std::vector<spdlog::sink_ptr> sinks;
	sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
	if (!config.log_file.empty())
		sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
			config.log_file));

	auto logger = std::make_shared<spdlog::logger>("exchange_tool",
												   sinks.begin(),
												   sinks.end());
	// %^…%$ colours the console sink; the file sink ignores the markers.
	logger->set_pattern("%^[%T.%e] [%l]%$ %v");
	logger->set_level(spdlog::level::from_str(config.log_level));
	logger->flush_on(spdlog::level::warn);
	spdlog::set_default_logger(std::move(logger));

	if (config.log_file.empty())
		spdlog::debug("logging initialised at level '{}' (console)",
					  config.log_level);
	else
		spdlog::debug("logging initialised at level '{}' (console + {})",
					  config.log_level,
					  config.log_file);
}