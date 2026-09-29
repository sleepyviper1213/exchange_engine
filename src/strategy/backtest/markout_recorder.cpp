#include "markout_recorder.hpp"

#include "markout_report.hpp"
#include "orders/types.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

namespace exchange::strategy::backtest {

markout_recorder::markout_recorder()
	: markout_recorder(std::span<const std::uint64_t>{DEFAULT_HORIZONS_NS}) {}

markout_recorder::markout_recorder(std::span<const std::uint64_t> horizons_ns) {
	for (const std::uint64_t horizon : horizons_ns) {
		if (horizon == 0) continue;
		if (horizon_count_ == markout_report::MAX_HORIZONS) break;
		// Strictly ascending is a precondition, not something to repair: the
		// per-horizon queues are each sorted by construction, and sorting the
		// list here would silently renumber the buckets a caller is about to
		// read by index.
		assert(
			(horizon_count_ == 0 || horizon > horizons_[horizon_count_ - 1]) &&
			"markout horizons must be strictly ascending");
		horizons_[horizon_count_]          = horizon;
		totals_[horizon_count_].horizon_ns = horizon;
		++horizon_count_;
	}
}

void markout_recorder::on_mid(std::uint64_t now_ns,
							  std::int64_t mid_half_ticks) noexcept {
	// Two passes, and the order between them is the whole no-look-ahead rule.
	// Everything due strictly before this frame prevailed under the *previous*
	// mid - that is the last observation at or before its deadline. Only once
	// that is banked does this frame's mid become current, and then deadlines
	// landing exactly on it are scored against it.
	if (has_mid_) resolve_due(now_ns, /*inclusive=*/false, last_mid_);

	last_mid_ = mid_half_ticks;
	has_mid_  = true;

	resolve_due(now_ns, /*inclusive=*/true, mid_half_ticks);
}

void markout_recorder::on_fill(std::uint64_t now_ns, side_t our_side,
							   price_t price, quantity_t volume,
							   bool is_passive) {
	if (volume <= 0) return;

	const waiting entry{
		.due_ns           = 0, // per horizon, below
		.price_half_ticks = 2 * static_cast<std::int64_t>(price),
		.volume           = static_cast<volume_t>(volume),
		.is_passive       = is_passive,
		.is_buy           = our_side == side_t::bid,
	};

	for (std::size_t i = 0; i < horizon_count_; ++i) {
		// Saturating: a stamp within one horizon of the end of the epoch is not
		// a real capture, but wrapping would make the deadline appear already
		// due and score the fill against the mid it just printed at.
		const std::uint64_t due = now_ns > UINT64_MAX - horizons_[i]
									  ? UINT64_MAX
									  : now_ns + horizons_[i];
		assert((pending_[i].empty() || pending_[i].back().due_ns <= due) &&
			   "fills must arrive in non-decreasing market time");
		waiting queued = entry;
		queued.due_ns  = due;
		pending_[i].push_back(queued);
	}
}

void markout_recorder::resolve_due(std::uint64_t as_of, bool inclusive,
								   std::int64_t mid) noexcept {
	for (std::size_t i = 0; i < horizon_count_; ++i) {
		auto &queue = pending_[i];
		while (!queue.empty()) {
			const std::uint64_t due = queue.front().due_ns;
			if (due > as_of || (!inclusive && due == as_of)) break;
			credit(queue.front(), i, mid);
			queue.pop_front();
		}
	}
}

void markout_recorder::credit(const waiting &fill, std::size_t horizon,
							  std::int64_t mid) noexcept {
	// Signed by our side: a buy profits when the mid rises above what we paid,
	// a sell when it falls below what we sold at. Both sides in half-ticks,
	// because `mid` is bid+ask rather than their average and `price_half_ticks`
	// was doubled to match. @see markout_report
	const std::int64_t move =
		fill.is_buy ? mid - fill.price_half_ticks : fill.price_half_ticks - mid;
	const std::int64_t weighted = move * fill.volume;

	markout_report::bucket &at = totals_[horizon];
	if (fill.is_passive) {
		++at.passive_fills;
		at.passive_lots += fill.volume;
		at.passive_half_tick_lots += weighted;
	} else {
		++at.aggressive_fills;
		at.aggressive_lots += fill.volume;
		at.aggressive_half_tick_lots += weighted;
	}
}

markout_report markout_recorder::finish() const {
	markout_report out;
	out.count = horizon_count_;
	for (std::size_t i = 0; i < horizon_count_; ++i) {
		out.horizons[i] = totals_[i];
		// Still queued means the capture ended before the horizon did. Counted,
		// never scored against the last mid that happened to exist - that would
		// be a shorter horizon reported under a longer one's name.
		out.horizons[i].unresolved =
			static_cast<std::uint64_t>(pending_[i].size());
	}
	return out;
}

std::size_t markout_recorder::pending() const noexcept {
	// The longest horizon drains last, so its queue holds every fill that still
	// has anything outstanding.
	return horizon_count_ == 0 ? 0 : pending_[horizon_count_ - 1].size();
}

std::size_t markout_recorder::horizon_count() const noexcept {
	return horizon_count_;
}

} // namespace exchange::strategy::backtest
