#include "owned_file.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <share.h>
#endif

namespace exchange::core::util {

owned_file open_shared(const std::filesystem::path &path, const char *mode) {
	// Narrowed once, into a named local. path::c_str() is wchar_t* on Windows,
	// which neither fopen takes, and calling .string() inline would hand a
	// pointer into a temporary to a function that outlives the argument.
	const std::string native = path.string();
#if defined(_WIN32)
	return owned_file(::_fsopen(native.c_str(), mode, _SH_DENYNO));
#else
	return owned_file(std::fopen(native.c_str(), mode));
#endif
}

} // namespace exchange::core::util
