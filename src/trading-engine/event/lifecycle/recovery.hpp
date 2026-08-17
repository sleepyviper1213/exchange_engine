#pragma once
// Recovery event.
//
// Follows a RECOVERED start-up and says what was rebuilt, from where, and how
// much of it. A session that inherits state is making a claim about the past —
// "these order ids still mean what they meant" — and this is the record that
// makes the claim checkable instead of assumed.
//
// Nothing emits one yet: the journal, the snapshot and the replay driver are
// TODO.md #6 and are not built. It is defined now anyway, because a log format
// has to be fixed *before* the log is written — a recovery that ran against a
// journal with no room to describe itself cannot be audited afterwards, and by
// then the journal it would have to describe is the one already on disk.

#include "core/util/enum_string.hpp"
#include "core/util/flag.hpp"
#include "fwd.hpp"

#include <cassert>
#include <cstdint>
#include <type_traits>

namespace exchange::engine::event::lifecycle {

/// @brief The two places rebuilt state can come from, one bit each.
#define RECOVERY_MODE_LIST(X)                                                \
	X(SNAPSHOT, 1U << 0U, "a checkpoint was loaded")                           \
	X(JOURNAL, 1U << 1U, "journal entries were re-applied")

enum class recovery_mode : std::uint8_t {
	EXCHANGE_ENUM_VALUED_VALUES(RECOVERY_MODE_LIST)
};

EXCHANGE_ENABLE_FLAGS(recovery_mode)

/// @brief The enumerator name of @p value, e.g. @c "SNAPSHOT".
EXCHANGE_ENUM_VALUED_NAME(recovery_mode, to_string, RECOVERY_MODE_LIST)

/// @brief What each source contributed, for logs.
EXCHANGE_ENUM_VALUED_LABEL_ONLY(recovery_mode, describe, RECOVERY_MODE_LIST)

#undef RECOVERY_MODE_LIST

/**
 * @brief Where a rebuild's state came from — one source, or both.
 *
 * The three combinations that mean anything, and how to read them:
 *
 * | set | what happened |
 * | --- | ------------- |
 * | @c SNAPSHOT | a checkpoint was loaded and nothing had happened since |
 * | @c JOURNAL | replayed from an empty book; no snapshot was available |
 * | @c SNAPSHOT|JOURNAL | a checkpoint, then the entries after it |
 *
 * The one worth alerting on is a bare @c JOURNAL. Replaying from nothing is
 * correct but costs the whole history, and a venue doing it has lost its
 * checkpoints — the recovery succeeded and the operator still needs to know.
 * The other two differ only in whether anything happened after the last
 * checkpoint, which for a venue stopped outside trading hours is legitimately
 * nothing.
 */
using recovery_modes = core::util::flag<recovery_mode>;

/**
 * @brief State was rebuilt from durable storage; the session continues an older
 *        one.
 *
 * @par Why it names the session it recovered from
 * Because that is the edge that makes a chain of logs one history. A start-up
 * says the mode was RECOVERED; only this says *which* session's ids are being
 * continued, and therefore which earlier log a reader has to have processed
 * before this one makes sense. Without it, two recovered sessions in an archive
 * are two unordered claims about a past neither of them identifies.
 *
 * @par Why the counts, again
 * Symmetric with @c shutdown's, and used the same way: a @c shutdown records
 * how much a session did, this records how much came back, and a recovery that
 * replayed fewer entries than the previous session published either found a
 * torn log or stopped early. Both are worth knowing at the moment it happens
 * rather than the first time a client asks why their resting order is gone.
 *
 * @c orders_restored is the count the rebuilt books should agree with, and is
 * the one number here that can be checked against live state immediately —
 * @c order_manager and the books are both countable the instant recovery ends.
 *
 * @par Why the timestamp is wall clock
 * @copydoc startup
 *
 * @invariant @c source is non-empty. An empty set would say the state was
 *            rebuilt out of nothing, which is a cold start — and a cold start
 *            emits a @c startup with @c StartMode::COLD and no recovery record
 *            at all. The empty set is representable because @c flag's default
 * is the empty one; @c is_well_formed is what says it is not a value this
 * record may carry.
 */
struct recovery {
	// Zero-initialised throughout, so a default-constructed record is the
	// well-defined not-a-recovery that is_well_formed() rejects rather than
	// indeterminate bytes a journal might append. Designated initialisers at
	// the call site are unaffected: an omitted field was already
	// value-initialised.
	session_id_t session        = 0;    ///< the session this recovery starts
	session_id_t recovered_from = 0;    ///< the session whose state was rebuilt
	std::uint64_t timestamp_ns  = 0;    ///< wall clock, ns since the UNIX epoch
	recovery_modes source;         ///< what the state was rebuilt out of
	std::uint64_t entries_replayed = 0; ///< journal entries re-applied
	std::uint64_t orders_restored =
		0; ///< resting orders the books came back with

	/// @brief Whether this record says something a recovery could have meant.
	/// @see the class invariant on why an empty @c source is not one.
	[[nodiscard]] constexpr bool is_well_formed() const noexcept {
		return !source.is_empty() && session != recovered_from;
	}

	bool operator==(const recovery &) const noexcept = default;
};

static_assert(
	std::is_trivially_copyable_v<recovery>,
	"a lifecycle record must stay trivially copyable so a journal "
	"append is a raw write; flag<E> is as wide as its underlying type "
	"and keeps that true");

} // namespace exchange::engine::event::lifecycle
