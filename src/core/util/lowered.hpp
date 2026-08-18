#pragma once
// ASCII case folding, kept here rather than in any one caller: lowercasing a
// venue symbol for a stream name is not market-data knowledge, it is a string
// operation that happens to be needed there.

#include "core_export.hpp" // CORE_EXPORT (generated)

#include <string>
#include <string_view>

namespace exchange::core::util {

/**
 * @brief An ASCII-lowercased copy of @p text.
 *
 * Folds per byte with an @c unsigned char argument, which is what @c std::tolower
 * requires - passing a plain @c char is undefined for values above 0x7F. The
 * copy is sized once from @p text and transformed in place, so the result costs
 * exactly one allocation and never grows.
 *
 * @param text Bytes to fold; may be empty.
 * @return A new string, the same length as @p text, with A-Z mapped to a-z.
 * @pre @p text is ASCII. Bytes outside 0x00-0x7F are passed through by the "C"
 *      locale but are not meaningfully case-folded, so this is not a Unicode
 *      lowercase and must not be used as one.
 * @post The result has the same size as @p text.
 */
[[nodiscard]] CORE_EXPORT std::string lowered(std::string_view text);

} // namespace exchange::core::util
