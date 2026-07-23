#pragma once
#include "util_export.h" // UTIL_EXPORT (generated)

#include <string>

namespace util {
/**
 * @brief Read an entire file into a string.
 * @param path Filesystem path to read.
 * @return The file contents (empty if the file is missing or empty).
 */
UTIL_EXPORT std::string slurp(const char *path);
}