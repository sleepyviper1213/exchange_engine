#include "lifecycle.hpp"

#include "core/logging/settings.hpp"

#include <fmt/format.h>
#include <spdlog/formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace exchange::core::logging {
namespace {

/**
 * @brief One JSON object per line: @c {"ts_ms":...,"level":"...",
 *        "logger":"...","msg":"..."}.
 *
 * @c msg.payload is the caller's formatted message, before spdlog's own
 * pattern would have decorated it — exactly the one field here that can
 * legitimately contain a quote, a backslash or a newline, so it is the one
 * field @c escape_json runs over. The other three are spdlog's own level
 * name and this process's logger name, neither of which a caller controls.
 *
 * @c ts_ms rather than a formatted timestamp: milliseconds since the epoch
 * is unambiguous across every consumer's timezone and locale, and needs no
 * `<fmt/chrono.h>` dependency to produce correctly on all four toolchains
 * this project builds with.
 */
class json_line_formatter final : public spdlog::formatter {
public:
	void format(const spdlog::details::log_msg &msg,
				spdlog::memory_buf_t &dest) final {
		const auto epoch_ms =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				msg.time.time_since_epoch())
				.count();
		fmt::format_to(fmt::appender(dest),
					   R"({{"ts_ms":{},"level":"{}","logger":"{}","msg":")",
					   epoch_ms,
					   spdlog::level::to_string_view(msg.level),
					   msg.logger_name);
		escape_json(msg.payload, dest);
		fmt::format_to(fmt::appender(dest), "\"}}\n");
	}

	[[nodiscard]] std::unique_ptr<spdlog::formatter> clone() const final {
		return std::make_unique<json_line_formatter>();
	}

private:
	/// @brief Append @p text to @p dest with the characters JSON forbids
	///        unescaped turned into their escape sequence; everything else is
	///        a plain byte copy, so an ordinary message pays almost nothing.
	static void escape_json(spdlog::string_view_t text,
							spdlog::memory_buf_t &dest) {
		for (const char c : text) {
			switch (c) {
			case '"': fmt::format_to(fmt::appender(dest), "\\\""); break;
			case '\\': fmt::format_to(fmt::appender(dest), "\\\\"); break;
			case '\n': fmt::format_to(fmt::appender(dest), "\\n"); break;
			case '\r': fmt::format_to(fmt::appender(dest), "\\r"); break;
			case '\t': fmt::format_to(fmt::appender(dest), "\\t"); break;
			default:
				// The rest of the C0 control range is legal UTF-8 but not
				// legal inside a JSON string unescaped.
				if (static_cast<unsigned char>(c) < 0x20)
					fmt::format_to(fmt::appender(dest),
								   "\\u{:04x}",
								   static_cast<unsigned char>(c));
				else dest.push_back(c);
			}
		}
	}
};

/// @brief The outcome of reading a level name, keeping "what it is" separate
///        from "was it what you asked for".
struct level_choice {
	spdlog::level::level_enum level = spdlog::level::info;
	/// False when @c logging_settings::level named nothing spdlog knows.
	bool recognised = true;
};

/**
 * @brief Turn a level name into a level.
 *
 * Exists as its own function because @c spdlog::level::from_str answers @c off
 * for anything it does not recognise, which makes a typo indistinguishable from
 * deliberately silencing the log — one wrong character in an env var and the
 * process runs with logging disabled and says nothing about it. Detecting that
 * needs the one thing from_str discards: whether the name was actually known.
 */
[[nodiscard]] level_choice read_level(const std::string &name) {
	const auto parsed = spdlog::level::from_str(name);
	if (parsed != spdlog::level::off || name == "off") return {parsed, true};
	return {spdlog::level::info, false};
}

/**
 * @brief Build the sink set @p config asks for.
 *
 * @param[out] degraded Empty when every requested sink was created; otherwise
 *        describes what was lost and why.
 *
 * Returning the problem instead of logging it is the point. This runs before
 * the logger it is building has been installed, so a log call here would go to
 * whatever default logger happened to already exist — spdlog's built-in one,
 * which writes to @b stdout. That would put a diagnostic in the stream reserved
 * for results, which is precisely the thing choosing a stderr sink was meant to
 * prevent. The caller emits it once the real logger is in place.
 */
[[nodiscard]] std::vector<spdlog::sink_ptr> make_sinks(const settings &config,
													   std::string &degraded) {
	std::vector<spdlog::sink_ptr> sinks;
	// stderr, not stdout. The log carries diagnostics; stdout carries the
	// program's actual result — a book ladder, a snapshot, a throughput figure.
	// Mixing them breaks the one thing a command-line tool owes its caller,
	// which is that redirecting stdout captures the result and nothing else.
	sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());

	if (config.log_file.empty()) return sinks;

	// A log file that cannot be opened throws. That must not take the process
	// with it: losing the file sink costs an audit trail, while failing to
	// start costs the run.
	try {
		sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(
			config.log_file));
	} catch (const std::exception &error) {
		degraded = fmt::format("log file '{}' unavailable ({}); stderr only",
							   config.log_file,
							   error.what());
	}
	return sinks;
}

/// @brief Assemble a logger over @p sinks with @p config's presentation and
///        buffering policy. Does not install it.
[[nodiscard]] std::shared_ptr<spdlog::logger>
make_logger(const settings &config, std::vector<spdlog::sink_ptr> sinks,
			spdlog::level::level_enum level) {
	auto logger = std::make_shared<spdlog::logger>(config.logger_name,
												   sinks.begin(),
												   sinks.end());
	if (config.structured) {
		// One escaped JSON object per line; see json_line_formatter above.
		// No colour markers — a structured consumer parses fields, not ANSI.
		logger->set_formatter(std::make_unique<json_line_formatter>());
	} else {
		// %^…%$ colours the console sink; the file sink ignores the markers.
		logger->set_pattern("%^[%T.%e] [%l]%$ %v");
	}
	logger->set_level(level);
	logger->flush_on(spdlog::level::warn);
	if (config.backtrace > 0) logger->enable_backtrace(config.backtrace);
	return logger;
}

} // namespace

void init(const settings &config) {
	std::string degraded;
	auto sinks                = make_sinks(config, degraded);
	const auto [level, known] = read_level(config.level);
	spdlog::set_default_logger(make_logger(config, std::move(sinks), level));

	// Both diagnostics are emitted only now, through the logger this call just
	// installed, so they land on the sinks the caller asked for rather than on
	// whichever logger happened to be default beforehand.
	if (!degraded.empty()) spdlog::warn("{}", degraded);
	if (!known)
		spdlog::warn("unknown log level '{}'; falling back to info",
					 config.level);

	if (config.log_file.empty())
		spdlog::debug("logging initialised at level '{}' (stderr)",
					  config.level);
	else
		spdlog::debug("logging initialised at level '{}' (stderr + {})",
					  config.level,
					  config.log_file);
}

void dump_backtrace() noexcept {
	try {
		if (const auto &logger = spdlog::default_logger())
			logger->dump_backtrace();
	} catch (...) {} // NOLINT(bugprone-empty-catch)
}

void flush() noexcept {
	// noexcept as documented: a failing flush is not worth taking the process
	// down for, and this runs from a destructor.
	try {
		if (const auto &logger = spdlog::default_logger()) logger->flush();
	} catch (...) {} // NOLINT(bugprone-empty-catch)
}

void shutdown() noexcept {
	flush();
	try {
		spdlog::shutdown();
	} catch (...) {} // NOLINT(bugprone-empty-catch)
}

} // namespace exchange::core::logging
