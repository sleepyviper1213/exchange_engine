// core::logging::settings::structured: one escaped JSON object per line
// instead of the human-readable pattern.

#include "core/logging.hpp"

#include <gtest/gtest.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using exchange::core::logging::guard;
using exchange::core::logging::settings;

namespace {

/**
 * @brief Point the default logger at a null sink, discarding every line.
 *
 * `order_test` never calls `logging::init`, so library code that logs - a
 * channel, or a direct `spdlog::warn` such as the one in `core_allocator` -
 * falls back to spdlog's own default logger, which writes *colourised* to
 * stdout. gtest writes to stdout too and neither takes a lock the other
 * respects, so a log line lands mid-banner and spdlog's colour escape stays
 * open across gtest's text: smeared colour under MSVC, and silently corrupted
 * lines under MinGW, where gtest emits no colour of its own to make it obvious.
 *
 * Tests assert on behaviour, not on log output. The one suite that does assert
 * on it installs its own logger below and puts this back afterwards.
 */
void silence_default_logger() {
	spdlog::set_default_logger(std::make_shared<spdlog::logger>(
		"order_test",
		std::make_shared<spdlog::sinks::null_sink_mt>()));
}

/**
 * @brief Ends logging before `main` returns, which `order_test` has nowhere
 *        else to do.
 */
class logging_environment : public ::testing::Environment {
public:
	void SetUp() override { silence_default_logger(); }

	void TearDown() override { exchange::core::logging::shutdown(); }
};

const ::testing::Environment *const LOGGING_ENV =
	::testing::AddGlobalTestEnvironment(new logging_environment);

TEST(LoggingStructured, EmitsOneEscapedJsonObjectPerLine) {
	const auto path = std::filesystem::temp_directory_path() /
					  "exchange_engine_structured_log_test.jsonl";
	std::filesystem::remove(path);

	{
		// Synchronous, against the settings default. This suite pins the JSON
		// envelope, not how a line reaches its sink, and the guard's destructor
		// only *enqueues* a flush on an async logger - spdlog's post_flush
		// returns without waiting on it - so the getline calls below would race
		// the backend thread instead of testing anything.
		const guard log{settings{.level       = "info",
								 .log_file    = path.string(),
								 .logger_name = "structured_test",
								 .structured  = true,
								 .async       = false}};
		spdlog::info("plain message");
		spdlog::warn("has a \"quote\", a\nnewline and a\ttab");
	}

	std::ifstream in(path);
	ASSERT_TRUE(in.is_open());
	std::string line;

	ASSERT_TRUE(static_cast<bool>(std::getline(in, line)));
	EXPECT_TRUE(line.contains(R"("level":"info")"));
	EXPECT_TRUE(line.contains(R"("logger":"structured_test")"));
	EXPECT_TRUE(line.contains(R"("msg":"plain message")"));
	EXPECT_TRUE(line.contains(R"("ts_ms":)"));

	ASSERT_TRUE(static_cast<bool>(std::getline(in, line)));
	// The quote and control characters in the message must be escaped, not
	// left to break the line's own JSON - an unescaped newline in particular
	// would split this one message across two lines in the file, which the
	// two getline calls above would already have shown as three lines total.
	EXPECT_TRUE(line.contains(R"(\"quote\")"));
	EXPECT_TRUE(line.contains(R"(a\nnewline)"));
	EXPECT_TRUE(line.contains(R"(a\ttab)"));
	EXPECT_FALSE(line.contains('\n'));

	// Exactly two lines: a third getline would mean the message's own
	// newline leaked through unescaped after all.
	EXPECT_FALSE(static_cast<bool>(std::getline(in, line)));
	in.close();

	// The guard's own shutdown has already run, and spdlog's registry keeps
	// every named logger alive by itself - replacing the *default* logger alone
	// would not have dropped "structured_test" from that table, so its file
	// sink would still be open and Windows would refuse to remove the file.
	// init() here hands the rest of this process a normal logger back so a
	// later test's spdlog:: call has something to log to; the environment above
	// is what ends it before main returns.
	exchange::core::logging::init(settings{.log_file = ""});
	std::filesystem::remove(path);

	// Back to quiet: with random scheduling, whatever runs after this must
	// not inherit a console logger. @see silence_default_logger
	silence_default_logger();
}

} // namespace
