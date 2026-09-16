#include "isolation.hpp"

#include "core/util/slurp.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace exchange::core::concurrency::affinity {
namespace {

constexpr std::string_view ISOLATION_BLANKS = " \t\n\r";

// The kernel's own ceiling is CONFIG_NR_CPUS, at most 8192 on x86-64. A list
// naming anything above it is malformed rather than describing a machine, and
// the cap is what stops a corrupt "0-4294967295" from being expanded one id at
// a time. NO_CORE (UINT_MAX) sits above it too, so the sentinel can never come
// back from a parse.
constexpr core_id ISOLATION_MAX_CPU = 8191;

[[nodiscard]] std::string_view trimmed(std::string_view text) noexcept {
	const auto first = text.find_first_not_of(ISOLATION_BLANKS);
	if (first == std::string_view::npos) return {};
	return text.substr(first,
					   text.find_last_not_of(ISOLATION_BLANKS) - first + 1);
}

/// Whole-token unsigned parse. The `ptr != end` check is the point: it rejects
/// "3x" and "domain" rather than accepting a numeric prefix, which is what
/// lets the caller treat "did not parse" as "this is a flag word, skip it".
[[nodiscard]] bool parse_cpu_id(std::string_view token, core_id &out) noexcept {
	if (token.empty()) return false;
	const char *const begin = token.data();
	const char *const end   = begin + token.size();
	unsigned value          = 0;
	const auto [ptr, ec]    = std::from_chars(begin, end, value);
	if (ec != std::errc{} || ptr != end || value > ISOLATION_MAX_CPU)
		return false;
	out = value;
	return true;
}

} // namespace

bool isolation::is_isolated(core_id id) const noexcept {
	return std::ranges::binary_search(isolated, id);
}

bool isolation::is_nohz_full(core_id id) const noexcept {
	return std::ranges::binary_search(nohz_full, id);
}

bool isolation::is_empty() const noexcept {
	return isolated.empty() && nohz_full.empty();
}

namespace detail {

std::vector<core_id> parse_cpu_list(std::string_view list) {
	std::vector<core_id> cpus;
	list = trimmed(list);
	// What an unset nohz_full reads back as - "(null)" on the kernels that
	// print the pointer and an empty file on the ones that do not.
	if (list.empty() || list == "(null)") return cpus;

	while (!list.empty()) {
		const auto comma             = list.find(',');
		const std::string_view token = trimmed(list.substr(0, comma));
		list = comma == std::string_view::npos ? std::string_view{}
											   : list.substr(comma + 1);
		if (token.empty()) continue;
		// "0-31:1/2" - every 1st CPU of each group of 2. Modelling that buys
		// nothing here and guessing it wrong hands out cores the kernel never
		// isolated, so the token is dropped whole.
		if (token.contains(':')) continue;

		const auto dash = token.find('-');
		core_id lo      = 0;
		core_id hi      = 0;
		if (dash == std::string_view::npos) {
			// A flag word ("domain", "managed_irq") fails here and is skipped.
			if (!parse_cpu_id(token, lo)) continue;
			hi = lo;
		} else if (!parse_cpu_id(trimmed(token.substr(0, dash)), lo) ||
				   !parse_cpu_id(trimmed(token.substr(dash + 1)), hi) ||
				   hi < lo) {
			continue;
		}
		for (core_id cpu = lo; cpu <= hi; ++cpu) cpus.push_back(cpu);
	}

	std::ranges::sort(cpus);
	const auto duplicates = std::ranges::unique(cpus);
	cpus.erase(duplicates.begin(), duplicates.end());
	return cpus;
}

std::string_view cmdline_value(std::string_view cmdline, std::string_view key) {
	if (key.empty()) return {};
	for (std::size_t pos = cmdline.find(key); pos != std::string_view::npos;
		 pos             = cmdline.find(key, pos + key.size())) {
		// Both ends have to be a boundary: a bare find() would answer
		// "isolcpus" out of "myisolcpus=" on the left and out of
		// "isolcpus_extra=" on the right.
		const bool starts_token =
			pos == 0 || ISOLATION_BLANKS.contains(cmdline[pos - 1]);
		const std::size_t assign = pos + key.size();
		if (!starts_token || assign >= cmdline.size() || cmdline[assign] != '=')
			continue;

		const std::size_t value = assign + 1;
		const std::size_t end = cmdline.find_first_of(ISOLATION_BLANKS, value);
		return end == std::string_view::npos
				   ? cmdline.substr(value)
				   : cmdline.substr(value, end - value);
	}
	return {};
}

} // namespace detail

#ifdef __linux__

isolation discover_isolation() {
	isolation iso;
	// sysfs first because it reports the *effective* set: it includes CPUs a
	// cpuset or systemd's CPUAffinity isolated after boot, and excludes ones
	// the boot parameter named but the machine does not have. /proc/cmdline is
	// only what was asked for, which makes it the fallback - for kernels
	// predating these files (pre-4.10 for `isolated`) - rather than the first
	// answer. Asking in the other order would report an isolation the running
	// kernel may have declined.
	iso.isolated = detail::parse_cpu_list(
		exchange::core::util::slurp("/sys/devices/system/cpu/isolated"));
	iso.nohz_full = detail::parse_cpu_list(
		exchange::core::util::slurp("/sys/devices/system/cpu/nohz_full"));

	if (iso.isolated.empty() || iso.nohz_full.empty()) {
		const std::string cmdline =
			exchange::core::util::slurp("/proc/cmdline");
		if (iso.isolated.empty())
			iso.isolated = detail::parse_cpu_list(
				detail::cmdline_value(cmdline, "isolcpus"));
		if (iso.nohz_full.empty())
			iso.nohz_full = detail::parse_cpu_list(
				detail::cmdline_value(cmdline, "nohz_full"));
	}
	return iso;
}

#else

// No equivalent to query. Windows can reserve CPUs at boot
// (bcdedit /set numproc, or a KeSetSystemAffinityThread-style reservation) but
// exposes no read-back of it, and macOS has no hard affinity at all - the
// platform where affinity.cpp already returns false. Reporting "nothing is
// isolated" is then the accurate answer rather than a degraded one.
isolation discover_isolation() { return {}; }

#endif

} // namespace exchange::core::concurrency::affinity
