#pragma once

#include <cstddef>

namespace exchange::engine::event {
// No class here carries a dll interface, and that is deliberate. Exporting a
// non-polymorphic class wholesale makes MSVC treat its *inline* members as part
// of the ABI - they stop being inlined across the boundary - and it makes every
// static constexpr member an imported object that no translation unit defines,
// which MinGW reports as an unresolved `__imp_` reference. So the annotation
// goes on the out-of-line public members instead, in the header that declares
// them. @see the Qt wiki's binary-compatibility rules.
struct level_change;
struct command;
struct journal_record;
struct symbol_run;
class engine_event;

/// @brief Default return-channel capacity, in events.
///
/// The same order of magnitude as @c engine_partition's command queue on
/// purpose: one command can publish several events (a sweep prints once per
/// level it takes, plus a lifecycle record per order it touches), so a return
/// ring materially smaller than the forward one would stall on a burst the
/// forward one absorbed.
inline constexpr std::size_t DEFAULT_EVENT_CAPACITY = 1U << 14;

/// @brief Default events an @c event_dispatcher takes per pump. Sized so the
///        three buffers it implies stay a few kilobytes - see the class note.
inline constexpr std::size_t DEFAULT_EVENT_BATCH = 128;

template <std::size_t Capacity = DEFAULT_EVENT_CAPACITY>
class event_channel;

} // namespace exchange::engine::event
