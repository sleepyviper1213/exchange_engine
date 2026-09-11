#include "weight_budget.hpp"

#include <algorithm>
#include <cassert>
#include <climits>

namespace exchange::venue {
namespace {

/// @p when as a count of whole seconds on the steady clock's epoch. Only
/// differences are ever used, so the epoch itself does not have to mean
/// anything.
[[nodiscard]] std::int64_t
whole_seconds(weight_budget::time_point when) noexcept {
	return std::chrono::duration_cast<std::chrono::seconds>(
			   when.time_since_epoch())
		.count();
}

} // namespace

weight_budget::weight_budget(int limit, std::chrono::seconds window)
	: limit_(limit),
	  window_(std::max(window, std::chrono::seconds{1})),
	  buckets_(static_cast<std::size_t>(window_.count()), 0) {
	assert(!buckets_.empty() && "a window shorter than a second has no bucket");
}

void weight_budget::advance(time_point when) noexcept {
	const std::int64_t now = whole_seconds(when);
	if (!started_) {
		started_ = true;
		second_  = now;
		return;
	}
	// Time only moves forward here. A caller handing back an earlier timestamp
	// than the last one is reporting out of order, and the budget stays where
	// it is rather than un-ageing spend that has already been counted.
	if (now <= second_) return;

	const std::int64_t elapsed = now - second_;
	const auto span            = static_cast<std::int64_t>(buckets_.size());
	if (elapsed >= span) {
		// The whole window aged out: nothing that was counted is still inside
		// it, so this is a reset rather than `span` separate evictions.
		std::ranges::fill(buckets_, 0);
		total_ = 0;
	} else {
		for (std::int64_t step = 0; step < elapsed; ++step) {
			at_ = (at_ + 1) % buckets_.size();
			total_ -= buckets_[at_];
			buckets_[at_] = 0;
		}
	}
	second_ = now;
}

void weight_budget::spend(int weight, time_point when) noexcept {
	advance(when);
	if (weight <= 0) return;
	buckets_[at_] += weight;
	total_ += weight;
}

void weight_budget::reconcile(int used, time_point when) noexcept {
	advance(when);
	// The venue reports a total, not a distribution, so the whole of it is
	// placed in the current bucket. @see the header for why over-counting the
	// age of that spend is the safe direction to be wrong in.
	std::ranges::fill(buckets_, 0);
	const int adopted = std::max(used, 0);
	buckets_[at_]     = adopted;
	total_            = adopted;
}

int weight_budget::used(time_point when) noexcept {
	advance(when);
	return static_cast<int>(std::min<std::int64_t>(total_, INT_MAX));
}

int weight_budget::remaining(time_point when) noexcept {
	if (is_unlimited()) return INT_MAX;
	return std::max(limit_ - used(when), 0);
}

bool weight_budget::can_spend(int weight, time_point when) noexcept {
	if (is_unlimited()) return true;
	return used(when) + std::max(weight, 0) <= limit_;
}

int weight_budget::limit() const noexcept { return limit_; }

std::chrono::seconds weight_budget::window() const noexcept { return window_; }

bool weight_budget::is_unlimited() const noexcept { return limit_ <= 0; }

} // namespace exchange::venue
