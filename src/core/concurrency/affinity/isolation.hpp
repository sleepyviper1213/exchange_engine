#pragma once

#include "core_export.hpp" // CORE_EXPORT (generated)
#include "types.hpp"

#include <string_view>
#include <vector>

// What the *kernel* has been told to keep off a CPU - the half of core
// placement that no amount of sched_setaffinity can buy.
//
// Pinning says "run this thread here". Isolation says "run nothing else here":
// a CPU in `isolcpus=` is removed from every scheduler domain at boot, so the
// load balancer never migrates work onto it and only a thread that names it
// explicitly - which is exactly what core_allocator does - ever runs there.
// That is the difference between a pinned thread that still shares its core
// with whatever the balancer parked on it and one that genuinely owns the core,
// and it is the difference docs/performance.md points at when it says the
// tens-of-microseconds `max_ns` on every benchmark family "is the OS preempting
// a pinned thread ... not the code".
//
// Scheduler isolation is not tick isolation, which is why both are read. A CPU
// that is isolcpus'd but not in `nohz_full=` still takes its periodic timer
// interrupt (CONFIG_HZ, typically 250 or 1000 a second) plus the RCU callbacks
// that come with it, and a 1 kHz interrupt on the matching core is a p99.9 the
// code did not earn. The two boot parameters are independent and an operator
// routinely sets one and forgets the other, so the process reports what it
// actually found rather than what it was presumably given. @see
// docs/deployment.md.
//
// Everything here is a startup query - one or two small sysfs reads - and it
// degrades to "nothing is isolated" on a kernel without the files and on every
// non-Linux host. A best-effort answer is the honest one: Windows has no
// equivalent (its closest analogue, a boot-time processor-affinity reservation,
// is not queryable this way), and a dev box has no isolation at all.
namespace exchange::core::concurrency::affinity {

/// @brief The kernel's CPU-isolation state, as two sorted, duplicate-free
///        lists of logical CPUs.
///
/// @note Empty lists mean "no isolation", never "unknown" - a caller cannot
///       distinguish an unisolated host from an unsupported one, and must not
///       need to: both mean it may not assume a core is undisturbed.
struct isolation {
	/// CPUs removed from the scheduler's domains (`isolcpus=`, or a cpuset
	/// with `sched_load_balance` off). Nothing runs here unless it pins here.
	std::vector<core_id> isolated;

	/// CPUs running tickless while a single runnable task occupies them
	/// (`nohz_full=`). A subset of the useful cores in practice, but the
	/// kernel enforces no relationship between the two lists.
	std::vector<core_id> nohz_full;

	/// @brief True when @p id is out of the scheduler's reach.
	[[nodiscard]] CORE_EXPORT bool is_isolated(core_id id) const noexcept;

	/// @brief True when @p id suppresses its periodic timer interrupt.
	[[nodiscard]] CORE_EXPORT bool is_nohz_full(core_id id) const noexcept;

	/// @brief True when the kernel isolated nothing - the dev-box case, and
	///        the one where a latency tail is the scheduler's rather than the
	///        engine's.
	[[nodiscard]] CORE_EXPORT bool is_empty() const noexcept;
};

/// @brief Read the host's isolation state. Never throws for platform reasons:
///        a missing file, an unreadable one or a non-Linux host all yield an
///        empty result, the same way @c discover() falls back to a flat
///        topology.
[[nodiscard]] CORE_EXPORT isolation discover_isolation();

namespace detail {

// The two parsers are the seam the tests drive - a test cannot boot a kernel,
// and the formats below are where this goes wrong in practice. They carry
// CORE_AUTOTEST_EXPORT for the same reason from_sibling_groups does: exported
// into a test build, absent from the shipping library's export table.

/// @brief Parse a kernel CPU list - @c "2-5,8", the format both
///        @c /sys/devices/system/cpu/isolated and @c isolcpus= use.
///
/// Tolerant by design, and deliberately *conservative* when it cannot be:
/// a token it does not fully understand is dropped rather than guessed at, so
/// an unparsed CPU is treated as not isolated. The cases that matter:
/// @li @c isolcpus= takes optional flag words before the list
///     (@c "domain,managed_irq,2-7"); non-numeric tokens are skipped.
/// @li the kernel's own cpulist grammar allows @c "0-31:1/2" (every 1st of
///     each 2) - dropped, because claiming the whole range would hand out
///     cores the kernel never isolated.
/// @li @c "(null)" is what an unset @c nohz_full reads back as on some
///     kernels, and an empty file is what it reads back as on others.
/// @li a reversed or malformed range yields nothing for that token.
/// @return Sorted, duplicate-free CPU ids.
[[nodiscard]] CORE_AUTOTEST_EXPORT std::vector<core_id>
parse_cpu_list(std::string_view list);

/// @brief The value of @p key in a @c /proc/cmdline -shaped string, or empty
///        if absent.
///
/// Matches on whole parameters only: looking up @c "isolcpus" must not match
/// the @c "nohz_full=..." next to it, nor a vendor's @c "myisolcpus=", so the
/// match has to begin at a token boundary. Quoted values are not unwrapped -
/// no CPU list needs quoting.
[[nodiscard]] CORE_AUTOTEST_EXPORT std::string_view
cmdline_value(std::string_view cmdline, std::string_view key);

} // namespace detail

} // namespace exchange::core::concurrency::affinity
