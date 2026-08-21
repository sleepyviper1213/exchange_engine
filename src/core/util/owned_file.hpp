#pragma once
// An owned C stdio handle: the RAII counterpart to slurp.hpp beside it.
//
// Two places in this tree needed one - the record log, which holds a journal
// open for the life of a partition, and the commands that overwrite a metrics
// exposition file - and each had grown its own. They were not the same shape,
// and the difference is worth keeping rather than averaging: one used a stateless
// closer, the other a pointer to `fclose`. This is the first, for the reason the
// static_assert below states.

#include "core_export.hpp" // CORE_EXPORT (generated)

#include <cstdio>
#include <filesystem>
#include <memory>

namespace exchange::core::util {

/**
 * @brief Closes a @c FILE*, for @c owned_file.
 *
 * Stateless on purpose. A @c unique_ptr whose deleter is an empty class stores
 * nothing for it - the empty base optimisation applies - and calls it directly;
 * a @c unique_ptr<FILE, decltype(&std::fclose)> has to carry the function
 * pointer, which doubles the handle's size and makes every close an indirect
 * call. Neither cost matters much for one file, and both are free to avoid.
 *
 * @note The null check is redundant against @c unique_ptr, which never calls a
 *       deleter on a null pointer. It is here so the closer is also correct when
 *       invoked directly, which is the only way it could be wrong.
 */
struct file_closer {
	void operator()(std::FILE *file) const noexcept {
		// NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
		if (file != nullptr) static_cast<void>(std::fclose(file));
	}
};

/// @brief A @c FILE* that closes itself. Null is a valid, empty handle, so a
///        failed open needs no special case at the call site.
using owned_file = std::unique_ptr<std::FILE, file_closer>;

static_assert(sizeof(owned_file) == sizeof(std::FILE *),
			  "an owned_file must cost no more than the handle it wraps; if "
			  "this fails the deleter has stopped being empty");

/**
 * @brief Open @p path with @p mode, allowing other handles on the same file.
 *
 * @param path The file. Narrowed to a byte string internally, because
 *        @c path::c_str() is @c wchar_t* on Windows and no @c fopen takes one.
 * @param mode A stdio mode string - @c "rb", @c "wb", @c "ab", @c "a+b",
 *        @c "r+b". Binary in every case worth using here: a journal is bytes,
 *        and a text-mode handle on Windows would translate them.
 * @return The handle, or an empty one if it could not be opened. Check against
 *         @c nullptr; @c errno carries the reason.
 *
 * @par Why this exists rather than an fstream
 * Because durability needs the descriptor underneath the handle. @c fsync on
 * POSIX and @c _commit on Windows both take one, and an @c std::ofstream has no
 * portable way to hand one over - so a stream can flush into the operating
 * system's cache and no further, which survives a process dying and loses data
 * to the machine dying. That distinction is the entire point of a journal.
 *
 * @par Why sharing, and why not fopen_s
 * The sharing is the reason this is one function rather than three. MSVC's
 * @c fopen_s opens for *exclusive* access, where POSIX @c fopen does not, so a
 * file opened that way cannot be read by anything else while the handle lives -
 * including a journal's own reader, and including whatever tails a metrics
 * exposition file. @c _fsopen with @c _SH_DENYNO restores the POSIX behaviour,
 * which is the one every caller here documents and expects. Three copies of this
 * split had drifted into two different answers; this is the one that is right.
 *
 * @note @c std::fopen is deprecated by MSVC and this project builds warnings as
 *       errors, so the platform split has to live somewhere. Once, here.
 */
[[nodiscard]] CORE_EXPORT owned_file
open_shared(const std::filesystem::path &path, const char *mode);

} // namespace exchange::core::util
