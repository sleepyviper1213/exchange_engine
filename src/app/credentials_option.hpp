#pragma once
// Reading the venue credential, in the one place that reads configuration.
//
// `venue/` owns the type and the variable names; `app/` owns the reading,
// because the composition root is where configuration enters and because this
// is the only module that already links a parser.
//
// --- why the option has no flag, and how ------------------------------------
//
// CLI11 binds an environment variable to an option with `->envname(...)`, which
// normally means the value has *two* sources: the variable and the flag. For a
// signing secret the flag is the problem - an argument lands in shell history
// and in every `ps` listing on the machine, and a key that has been there
// cannot be un-leaked.
//
// An option constructed with an empty name has no long name, no short name and
// no positional slot, so `envname` becomes its only identity and its only
// source. Verified against CLI11 2.6.1: such an option reports
// `lnames=0 snames=0 positional=0`, takes its value from the environment, and a
// stray positional argument is *rejected* rather than consumed by it.

#include "venue/credentials.hpp"

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include <string_view>

namespace exchange::app {

/**
 * @brief Attach environment-only options filling @p into to @p command.
 *
 * @param command The subcommand that may need to authenticate.
 * @param key_var, secret_var Which variables to read. Defaulted to the ones
 *        @c venue/credentials.hpp names, and parameters only so a test can
 *        point at variables it knows are unset: the process environment is
 *        global state, and a test that asserts on what a *developer's shell*
 *        happens to hold passes on one machine and fails on the next.
 * @param into Storage for the credential. Must outlive the parse - CLI11 keeps
 *        a pointer, exactly as every other option in @c cli.cpp does.
 *
 * @note Neither option appears in @c --help output beyond its environment
 *       name, and neither can be set from the command line. A run with the
 *       variables unset gets an empty credential rather than an error, because
 *       most commands here do not authenticate: only the ones that place
 *       orders need to check @c credentials::is_complete.
 */
inline void
add_credentials(CLI::App &command, venue::credentials &into,
				std::string_view key_var    = venue::API_KEY_VAR,
				std::string_view secret_var = venue::API_SECRET_VAR) {
	command.add_option("", into.key, "Venue API key (environment only)")
		->envname(std::string(key_var));
	command.add_option("", into.secret, "Venue API secret (environment only)")
		->envname(std::string(secret_var));
}

/**
 * @brief One line describing @p creds for a startup log.
 *
 * @return The key's fingerprint when both halves are present, or a statement
 *         that order entry is unavailable - which is the operationally useful
 *         thing to say, because a run that silently cannot trade looks
 *         identical to one that can until it tries.
 * @note Never renders the secret, and never renders more of the key than
 *       @c credentials::fingerprint allows.
 */
[[nodiscard]] inline std::string describe(const venue::credentials &creds) {
	if (!creds.is_complete())
		return "no venue credential configured - order entry is disabled";
	// A key too short to fingerprint is too short to be a key: a venue issues
	// tens of characters, and four means the variable holds something that is
	// not one. Said as a problem rather than reported as a fingerprint, because
	// the alternative is an unexplained HTTP 400 a minute later.
	if (creds.fingerprint().starts_with("<"))
		return fmt::format("{} {} - the variable holds something that is not a "
						   "key; check how it was set",
						   venue::API_KEY_VAR,
						   creds.fingerprint());
	return fmt::format("venue credential {} (key set, secret set)",
					   creds.fingerprint());
}

} // namespace exchange::app
