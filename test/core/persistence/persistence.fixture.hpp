#pragma once
// A throwaway directory on disk, for the suites that need real files.
//
// Persistence is the one part of this tree that cannot be tested against an
// in-memory double and still be testing anything: the properties are about what
// survives a process, which is a question about a filesystem. So these suites use
// real files, and this is what keeps them from leaving any behind.
//
// It lives here rather than beside each suite because the trading-engine's
// journal suite needs it too — the test tree is on the include path, so a fixture
// crosses module folders by being included by path. @see test/CMakeLists.txt

#include <filesystem>
#include <string>
#include <system_error>

/**
 * @brief A uniquely-named directory under the system temp, removed on exit.
 *
 * Named per suite rather than randomly, and cleared on the way *in* as well as
 * out: a run that crashed half way leaves its directory behind, and the next run
 * has to start from empty rather than inheriting whatever the crash wrote.
 *
 * Every operation swallows its error code on purpose. A cleanup that throws from
 * a destructor would replace a real test failure with a confusing one, and a
 * cleanup that fails is not itself the thing under test.
 */
class scratch_dir {
public:
	explicit scratch_dir(const std::string &name)
		: path_(std::filesystem::temp_directory_path() /
				("exchange_test_" + name)) {
		std::error_code ec;
		std::filesystem::remove_all(path_, ec);
		std::filesystem::create_directories(path_, ec);
	}

	scratch_dir(const scratch_dir &)            = delete;
	scratch_dir &operator=(const scratch_dir &) = delete;
	scratch_dir(scratch_dir &&)                 = delete;
	scratch_dir &operator=(scratch_dir &&)      = delete;

	~scratch_dir() {
		std::error_code ec;
		std::filesystem::remove_all(path_, ec);
	}

	/// @brief A path to @p leaf inside this directory. The file need not exist.
	[[nodiscard]] std::filesystem::path file(const char *leaf) const {
		return path_ / leaf;
	}

	[[nodiscard]] const std::filesystem::path &path() const noexcept {
		return path_;
	}

private:
	std::filesystem::path path_;
};
