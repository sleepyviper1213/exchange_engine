#pragma once

#include "core/logging/fwd.hpp"
#include "core/metrics/fwd.hpp"

namespace CLI {
class App;
}

namespace exchange::app {

/// Read when @c --config names nothing. Absent is not an error.
inline constexpr const char *DEFAULT_CONFIG_PATH = "exchange_tool.ini";

/**
 * @brief Declare the settings @p app accepts, and where it reads them from.
 *
 * Precedence, lowest to highest:
 *
 *     built-in defaults  <  INI file  <  command-line arguments
 *
 * All of it is CLI11's, via @c set_config: an option takes its value from the
 * config file if the file names it, and from the command line if the command
 * line names it, with the command line winning. There is nothing here that
 * merges sources by hand, because there is nothing to merge — every setting is
 * one option with one home.
 *
 * @param app The application, before parsing.
 * @param[out] log_settings Bound to the logging options, so it holds the
 *        resolved values once @p app has been parsed — and only then.
 * @param[out] metrics_settings Bound to the metrics options the same way.
 *        Metrics are off by default (@c core::metrics::settings::enabled),
 *        so a caller that never mentions @c --metrics-* gets exactly the old
 *        behaviour.
 *
 * @code{.ini}
 * # exchange_tool.ini -- keys are option names without the dashes
 * log-level           = debug
 * log-file            = exchange_tool.log
 * log-backtrace       = 64
 * log-json            = true
 * metrics-enabled     = true
 * metrics-file        = exchange_tool_metrics.prom
 * metrics-interval-ms = 1000
 * @endcode
 *
 * @code{.sh}
 * exchange_tool --config prod.ini --log-level warn replay sol.jsonl
 * @endcode
 *
 * Flat keys, no @c [logging] / @c [metrics] section: CLI11 maps a config
 * section to a @b subcommand of that name, so a section would mean declaring
 * a subcommand the tool does not have and would then appear to accept.
 *
 * A bad value is a parse error — @c --log-level is checked against the names
 * spdlog knows, and @c --log-backtrace / @c --metrics-interval-ms against
 * being a number — so it is reported with a usage message and an exit code,
 * before anything runs. That is CLI11's behaviour and it applies equally to a
 * value that came from the file.
 */
void add_configuration(CLI::App &app, core::logging::settings &log_settings,
					   core::metrics::settings &metrics_settings);

} // namespace exchange::app
