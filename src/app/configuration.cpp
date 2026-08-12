#include "configuration.hpp"

#include <string>

namespace exchange::app {

void add_configuration(CLI::App &app, core::logging::settings &log_settings,
					   core::metrics::settings &metrics_settings) {
	app.set_config("--config",
				   DEFAULT_CONFIG_PATH,
				   "INI file to read settings from",
				   false);
	app.add_option("--log-level", log_settings.level, "Verbosity")
		->check(CLI::IsMember(
			{"trace", "debug", "info", "warn", "error", "critical", "off"}))
		->capture_default_str()
		->group("Logging");
	app.add_option("--log-file", log_settings.log_file,
				   "Log file; empty disables the file sink")
		->capture_default_str()
		->group("Logging");
	app.add_option("--log-name", log_settings.logger_name,
				   "Logger name, as it appears in the log pattern")
		->capture_default_str()
		->group("Logging");
	app.add_option("--log-backtrace", log_settings.backtrace,
				   "Messages held back for a dump on failure; 0 disables")
		->capture_default_str()
		->group("Logging");

	app.add_flag("--metrics-enabled", metrics_settings.enabled,
				"Periodically write a Prometheus text exposition file")
		->capture_default_str()
		->group("Metrics");
	app.add_option("--metrics-file", metrics_settings.output_file,
				   "Path overwritten with the current metrics on every "
				   "interval")
		->capture_default_str()
		->group("Metrics");
	app.add_option("--metrics-interval-ms", metrics_settings.interval_ms,
				   "How often the metrics file is rewritten")
		->capture_default_str()
		->group("Metrics");
}

} // namespace exchange::app
