#pragma once

#include <string_view>

#ifndef EXCHANGE_ENGINE_VERSION
#define EXCHANGE_ENGINE_VERSION "0.0.0-dev"
#endif
#ifndef EXCHANGE_ENGINE_VERSION_MAJOR
#define EXCHANGE_ENGINE_VERSION_MAJOR 0
#endif
#ifndef EXCHANGE_ENGINE_VERSION_MINOR
#define EXCHANGE_ENGINE_VERSION_MINOR 0
#endif
#ifndef EXCHANGE_ENGINE_VERSION_PATCH
#define EXCHANGE_ENGINE_VERSION_PATCH 0
#endif

namespace core {

inline constexpr std::string_view version = EXCHANGE_ENGINE_VERSION;
inline constexpr int version_major        = EXCHANGE_ENGINE_VERSION_MAJOR;
inline constexpr int version_minor        = EXCHANGE_ENGINE_VERSION_MINOR;
inline constexpr int version_patch        = EXCHANGE_ENGINE_VERSION_PATCH;

} // namespace core
