#include "configuration.hpp"

#include <string>

namespace exchange::app {

void add_configuration(CLI::App &app, core::logging::settings &settings) {
	app.set_config("--config",
				   DEFAULT_CONFIG_PATH,
				   "INI file to read settings from",
				   false);
	app.add_option("--log-level", settings.level, "Verbosity")
		->check(CLI::IsMember(
			{"trace", "debug", "info", "warn", "error", "critical", "off"}))
		->capture_default_str()
		->group("Logging");
	app.add_option("--log-file", settings.log_file,
				   "Log file; empty disables the file sink")
		->capture_default_str()
		->group("Logging");
	app.add_option("--log-name", settings.logger_name,
				   "Logger name, as it appears in the log pattern")
		->capture_default_str()
		->group("Logging");
	app.add_option("--log-backtrace", settings.backtrace,
				   "Messages held back for a dump on failure; 0 disables")
		->capture_default_str()
		->group("Logging");
}

} // namespace exchange::app
