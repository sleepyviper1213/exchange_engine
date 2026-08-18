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

TEST(LoggingStructured, EmitsOneEscapedJsonObjectPerLine) {
	const auto path = std::filesystem::temp_directory_path() /
					  "exchange_engine_structured_log_test.jsonl";
	std::filesystem::remove(path);

	{
		const guard log{settings{.level       = "info",
								 .log_file    = path.string(),
								 .logger_name = "structured_test",
								 .structured  = true}};
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

	// The guard above only flushed (see its own class note on why it does not
	// call shutdown), and spdlog's registry keeps every named logger alive by
	// itself - replacing the *default* logger alone does not drop
	// "structured_test" from that table, so its file sink stays open.
	// shutdown() is what actually releases it (registry::shutdown clears the
	// table), which Windows requires before the file can be removed; init()
	// right after hands the rest of this process a normal logger back so a
	// later test's spdlog:: call has something to log to.
	exchange::core::logging::shutdown();
	exchange::core::logging::init(settings{.log_file = ""});
	std::filesystem::remove(path);
}

} // namespace
