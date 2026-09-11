#include "app/credentials_option.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

// The credential's only entry point into the process.
//
// What is pinned here is a security property, not a convenience: the secret has
// no command-line path. An argument lands in shell history and in every `ps`
// listing on the machine, and a key that has been there cannot be un-leaked -
// so an option that could accept one would be the bug, however it was spelled.

using exchange::app::add_credentials;
using exchange::app::describe;
using exchange::venue::credentials;

namespace {

/// Variables no shell will have set.
///
/// The suite used the real names and passed - until a developer exported a key
/// to run `trade`, at which point CLI11 read it and two cases asserting on an
/// empty credential failed. The process environment is global state, and a test
/// that reads whatever the developer's shell happens to hold is a test that
/// passes on one machine and fails on the next. Naming variables nobody sets is
/// how this suite states the environment it wants without mutating anyone's.
constexpr auto TEST_KEY_VAR    = "EXCHANGE_TEST_ABSENT_API_KEY";
constexpr auto TEST_SECRET_VAR = "EXCHANGE_TEST_ABSENT_API_SECRET";

/// A parser carrying nothing but the credential options, so a case states the
/// argv it wants without every `serve` flag having to be satisfied.
class credentials_option_app {
public:
	/// @param key_var, secret_var Which variables the options read. Defaulted
	///        to ones that are never set, so a case asserting on an empty
	///        credential asserts on this suite rather than on the machine.
	explicit credentials_option_app(
		std::string_view key_var    = TEST_KEY_VAR,
		std::string_view secret_var = TEST_SECRET_VAR) {
		add_credentials(app_, creds_, key_var, secret_var);
	}

	/// Parse @p args as if they followed the program name.
	/// @return Whether CLI11 accepted them.
	[[nodiscard]] bool parse(const std::vector<std::string> &args) {
		// CLI11 consumes its vector back-to-front, which is why the arguments
		// are handed over reversed rather than in the order they read here.
		std::vector<std::string> reversed(args.rbegin(), args.rend());
		try {
			app_.parse(reversed);
		} catch (const CLI::ParseError &) { return false; }
		return true;
	}

	[[nodiscard]] const credentials &value() const & noexcept { return creds_; }

	[[nodiscard]] CLI::App &app() noexcept { return app_; }

private:
	CLI::App app_{"credentials-option-test"};
	credentials creds_{};
};

} // namespace

TEST(CredentialsOption, NeitherOptionHasAFlagOrAPositionalSlot) {
	credentials_option_app harness;

	const std::vector<CLI::Option *> options = harness.app().get_options();
	int env_only                             = 0;
	for (const CLI::Option *option : options) {
		if (option->get_envname().empty()) continue;
		++env_only;
		// The three ways CLI11 can take a value off the command line, all
		// absent. If any of these becomes non-zero the secret has grown a flag.
		EXPECT_TRUE(option->get_lnames().empty()) << option->get_envname();
		EXPECT_TRUE(option->get_snames().empty()) << option->get_envname();
		EXPECT_FALSE(option->get_positional()) << option->get_envname();
	}
	EXPECT_EQ(env_only, 2) << "expected exactly the key and the secret";
}

TEST(CredentialsOption, TheOptionsAreNamedOnlyByTheirEnvironmentVariables) {
	// The real names, because this is the case that pins them - and it only
	// reads the option's spelling, never parses, so nothing is read from the
	// environment whatever the shell holds.
	credentials_option_app harness{exchange::venue::API_KEY_VAR,
								   exchange::venue::API_SECRET_VAR};

	std::vector<std::string> names;
	for (const CLI::Option *option : harness.app().get_options())
		if (!option->get_envname().empty())
			names.push_back(option->get_envname());

	ASSERT_EQ(names.size(), 2U);
	EXPECT_EQ(names[0], exchange::venue::API_KEY_VAR);
	EXPECT_EQ(names[1], exchange::venue::API_SECRET_VAR);
}

TEST(CredentialsOption, APositionalArgumentIsRefusedRatherThanConsumed) {
	credentials_option_app harness;

	// The failure mode this design exists to prevent: someone types the secret
	// after the subcommand and it is quietly accepted. CLI11 must reject it,
	// because no option is willing to take a positional.
	EXPECT_FALSE(harness.parse({"a-secret-typed-by-hand"}));
	EXPECT_TRUE(harness.value().secret.empty());
	EXPECT_TRUE(harness.value().key.empty());
}

TEST(CredentialsOption, AnUnsetEnvironmentYieldsAnEmptyCredential) {
	credentials_option_app harness;

	// Not an error: most commands here never authenticate, so an absent
	// credential must not stop a depth capture from running. Reads variables
	// nobody sets, so "unset" is a fact about the suite rather than a hope
	// about the machine. @see TEST_KEY_VAR
	ASSERT_TRUE(harness.parse({}));
	EXPECT_FALSE(harness.value().is_complete());
}

TEST(CredentialsOption, DescribeSaysOrderEntryIsOffWithoutACredential) {
	EXPECT_EQ(describe(credentials{}),
			  "no venue credential configured - order entry is disabled");
	// Half a credential is not a weaker one, it is none.
	EXPECT_EQ(describe(credentials{.key = "abcdefgh", .secret = ""}),
			  "no venue credential configured - order entry is disabled");
}

TEST(CredentialsOption, AKeyTooShortToBeOneIsReportedAsAProblem) {
	// A venue issues tens of characters. Four means the variable holds
	// something that is not a key - a truncated shell capture, a placeholder -
	// and saying so beats the unexplained HTTP 400 the venue answers with a
	// round trip later.
	const credentials stub{.key = "abc", .secret = "s"};
	const std::string line = describe(stub);

	EXPECT_TRUE(line.contains("not a key")) << line;
	EXPECT_TRUE(line.contains(exchange::venue::API_KEY_VAR)) << line;
	// And still never the secret.
	EXPECT_FALSE(line.contains("s\"")) << line;
}

TEST(CredentialsOption, DescribeNeverRendersTheSecretOrTheWholeKey) {
	const credentials creds{.key    = "abcdefghijklmnop",
							.secret = "topsecretvalue"};
	const std::string line = describe(creds);

	EXPECT_FALSE(line.contains("topsecretvalue")) << line;
	EXPECT_FALSE(line.contains("abcdefghijklmnop")) << line;
	// Enough to tell two configured keys apart, and no more.
	EXPECT_TRUE(line.contains("abcd...")) << line;
}

TEST(CredentialsOption, AShortKeyIsNotPartiallyRevealed) {
	// A four-character key would be shown whole by a naive prefix, so the
	// fingerprint refuses rather than trimming to nothing useful. Bound to a
	// local because a braced initialiser's comma would split the macro's
	// arguments.
	const credentials shortish{.key = "abcd", .secret = "s"};
	EXPECT_EQ(shortish.fingerprint(), "<too short to be a key>");
	EXPECT_EQ(credentials{}.fingerprint(), "<unset>");
}
