#include "cache_probe.hpp"

#include <cstdint>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <vector>
#include <windows.h>

#elifdef __APPLE__
#include <sys/sysctl.h>
#include <sys/types.h>
#elifdef __linux__
#include <unistd.h>
#endif

namespace exchange::core::concurrency {
namespace {
// Big enough to be a useful clamp on any host this runs on, small enough that
// a caller that believes it does not thrash a machine that has less.
constexpr std::size_t CACHE_SIZE_FALLBACK = 8U * 1024U * 1024U;
// The line every platform here actually uses, and what CACHE_LINE_SIZE assumes
// when the standard library will not say. A host that reports nothing is
// assumed to match rather than to differ.
constexpr std::size_t LINE_SIZE_FALLBACK = 64U;
} // namespace

#ifdef _WIN32

std::size_t last_level_cache_size() noexcept {
	DWORD len = 0;
	// Deliberately unchecked: the first call is expected to fail with
	// ERROR_INSUFFICIENT_BUFFER and its job is to set len.
	GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
	if (len == 0) return CACHE_SIZE_FALLBACK;

	std::vector<std::byte> buffer(len);
	if (GetLogicalProcessorInformationEx(
			RelationCache,
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
				buffer.data()),
			&len) == 0)
		return CACHE_SIZE_FALLBACK;

	// Deepest data/unified cache present; an instruction cache never holds the
	// line a prefetch is warming.
	BYTE deepest    = 0;
	std::size_t out = 0;
	for (std::byte *ptr = buffer.data(); ptr < buffer.data() + len;) {
		auto *info =
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
		if (info->Relationship == RelationCache &&
			(info->Cache.Type == CacheUnified ||
			 info->Cache.Type == CacheData) &&
			info->Cache.Level > deepest) {
			deepest = info->Cache.Level;
			out     = info->Cache.CacheSize;
		}
		ptr += info->Size;
	}
	return out > 0 ? out : CACHE_SIZE_FALLBACK;
}

std::size_t cache_line_size() noexcept {
	DWORD len = 0;
	GetLogicalProcessorInformationEx(RelationCache, nullptr, &len);
	if (len == 0) return LINE_SIZE_FALLBACK;

	std::vector<std::byte> buffer(len);
	if (GetLogicalProcessorInformationEx(
			RelationCache,
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
				buffer.data()),
			&len) == 0)
		return LINE_SIZE_FALLBACK;

	// The shallowest data/unified cache - L1d is where coherence and false
	// sharing happen, and the deeper levels share its line size on every
	// platform this builds for.
	BYTE shallowest = 0;
	std::size_t out = 0;
	for (std::byte *ptr = buffer.data(); ptr < buffer.data() + len;) {
		auto *info =
			reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr);
		if (info->Relationship == RelationCache &&
			(info->Cache.Type == CacheUnified ||
			 info->Cache.Type == CacheData) &&
			(shallowest == 0 || info->Cache.Level < shallowest)) {
			shallowest = info->Cache.Level;
			out        = info->Cache.LineSize;
		}
		ptr += info->Size;
	}
	return out > 0 ? out : LINE_SIZE_FALLBACK;
}

#elif defined(__APPLE__)

std::size_t last_level_cache_size() noexcept {
	// hw.l3cachesize is absent or 0 on Apple silicon, so fall back through the
	// levels rather than reporting nothing.
	for (const char *name :
		 {"hw.l3cachesize", "hw.l2cachesize", "hw.l1dcachesize"}) {
		std::int64_t size = 0;
		std::size_t len   = sizeof(size);
		if (sysctlbyname(name, &size, &len, nullptr, 0) == 0 && size > 0)
			return static_cast<std::size_t>(size);
	}
	return CACHE_SIZE_FALLBACK;
}

std::size_t cache_line_size() noexcept {
	std::int64_t size = 0;
	std::size_t len   = sizeof(size);
	if (sysctlbyname("hw.cachelinesize", &size, &len, nullptr, 0) == 0 &&
		size > 0)
		return static_cast<std::size_t>(size);
	return LINE_SIZE_FALLBACK;
}

#elif defined(__linux__)

std::size_t last_level_cache_size() noexcept {
	for (const int name :
		 {_SC_LEVEL3_CACHE_SIZE, _SC_LEVEL2_CACHE_SIZE, _SC_LEVEL1_DCACHE_SIZE})
		if (const long size = sysconf(name); size > 0)
			return static_cast<std::size_t>(size);
	return CACHE_SIZE_FALLBACK;
}

std::size_t cache_line_size() noexcept {
	if (const long size = sysconf(_SC_LEVEL1_DCACHE_LINESIZE); size > 0)
		return static_cast<std::size_t>(size);
	return LINE_SIZE_FALLBACK;
}

#else

std::size_t last_level_cache_size() noexcept { return CACHE_SIZE_FALLBACK; }

std::size_t cache_line_size() noexcept { return LINE_SIZE_FALLBACK; }

#endif

} // namespace exchange::core::concurrency
