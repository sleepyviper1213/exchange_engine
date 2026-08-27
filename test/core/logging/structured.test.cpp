// core::logging::settings::structured: one escaped JSON object per line
// instead of the human-readable pattern.

#include "core/logging.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

using exchange::core::logging::guard;
using exchange::core::logging::settings;

namespace {

/**
 * @brief Ends logging before `main` returns, which `order_test` has nowhere
 *        else to do.
 *
 * A normal program declares a `logging::guard` in `main` and its destructor
 * carries this. This binary's `main` belongs to gtest, so an async logger
 * installed by a test would otherwise survive to *static* destruction - where
 * joining its worker thread deadlocks against the Windows loader lock and the
 * process hangs after the suite has already reported success. A global
 * environment tears down inside `RUN_ALL_TESTS`, on an ordinary running
 * thread, which is exactly where that join completes.
 *
 * Registered here rather than in a file of its own because this is the only
 * suite in the tree that installs a logger; `shutdown` is a no-op when none
 * was, so it costs nothing when this suite is filtered out.
 */
class logging_shutdown_environment : public ::testing::Environment {
public:
	void TearDown() override { exchange::core::logging::shutdown(); }
};

[[maybe_unused]] const ::testing::Environment *const LOGGING_SHUTDOWN_ENV =
	::testing::AddGlobalTestEnvironment(new logging_shutdown_environment);

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
}

} // namespace
