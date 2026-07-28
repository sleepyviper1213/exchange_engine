#pragma once
#include "core_export.hpp" // UTIL_EXPORT (generated)

#include <string>
#include <filesystem>

namespace exchange::core::util{
/**
 * @brief Read an entire file into a string.
 * @param path Filesystem path to read.
 * @return The file contents (empty if the file is missing or empty).
 */
CORE_EXPORT std::string slurp(const std::filesystem::path &path);
}