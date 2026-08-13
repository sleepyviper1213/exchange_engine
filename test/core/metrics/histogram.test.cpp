#include "core/metrics/histogram.hpp"

#include <gtest/gtest.h>

#include <cstdint>

using exchange::core::metrics::histogram;
using exchange::core::metrics::latency_budgets;

namespace {

TEST(MetricsHistogram, AFreshHistogramHasNoObservations) {
	const histogram h;
	const histogram::snapshot snap = h.read();
	EXPECT_EQ(snap.total, 0U);
	EXPECT_EQ(snap.quantile(0.5), 0U);
}

TEST(MetricsHistogram, UpperBoundIsOnePastTheHighestValueTheBucketHolds) {
	EXPECT_EQ(histogram::upper_bound(0), 0U);  // bucket 0: exactly 0
	EXPECT_EQ(histogram::upper_bound(1), 1U);  // bucket 1: [1, 1]
	EXPECT_EQ(histogram::upper_bound(2), 3U);  // bucket 2: [2, 3]
	EXPECT_EQ(histogram::upper_bound(3), 7U);  // bucket 3: [4, 7]
	EXPECT_EQ(histogram::upper_bound(64), ~std::uint64_t{0});
}

TEST(MetricsHistogram, RecordPlacesAValueInTheBucketItsBitWidthNames) {
	histogram h;
	h.record(0); // bucket 0
	h.record(5); // bit_width(5) == 3 -> bucket 3, [4,7]

	const histogram::snapshot snap = h.read();
	EXPECT_EQ(snap.counts[0], 1U);
	EXPECT_EQ(snap.counts[3], 1U);
	EXPECT_EQ(snap.total, 2U);
}

TEST(MetricsHistogram, QuantileOfASingleValueReturnsItsBucketsUpperBound) {
	histogram h;
	for (int i = 0; i < 10; ++i) h.record(100); // bit_width(100) == 7

	const histogram::snapshot snap = h.read();
	EXPECT_EQ(snap.quantile(0.0), histogram::upper_bound(7));
	EXPECT_EQ(snap.quantile(0.5), histogram::upper_bound(7));
	EXPECT_EQ(snap.quantile(0.99), histogram::upper_bound(7));
}

TEST(MetricsHistogram, QuantileWalksCumulativeCountsAcrossBuckets) {
	histogram h;
	// 90 fast observations in bucket 1 ([1,1]), 10 slow ones in bucket 10
	// ([512,1023]) — p50 should land in the fast bucket, p99 in the slow one.
	for (int i = 0; i < 90; ++i) h.record(1);
	for (int i = 0; i < 10; ++i) h.record(1000);

	const histogram::snapshot snap = h.read();
	EXPECT_EQ(snap.total, 100U);
	EXPECT_EQ(snap.quantile(0.50), histogram::upper_bound(1));
	EXPECT_EQ(snap.quantile(0.99), histogram::upper_bound(10));
}

TEST(MetricsHistogram, IsHealthyWhenEveryConfiguredQuantileIsUnderBudget) {
	histogram h{latency_budgets{.p99_ns = 1023}};
	for (int i = 0; i < 90; ++i) h.record(1);
	for (int i = 0; i < 10; ++i) h.record(1000); // bit_width(1000) -> [512,1023]

	EXPECT_TRUE(h.is_healthy());
}

TEST(MetricsHistogram, IsUnhealthyWhenAConfiguredQuantileExceedsItsBudget) {
	histogram h{latency_budgets{.p99_ns = 1022}}; // one under the p99 bucket
	for (int i = 0; i < 90; ++i) h.record(1);
	for (int i = 0; i < 10; ++i) h.record(1000);

	EXPECT_FALSE(h.is_healthy());
}

TEST(MetricsHistogram, DisabledBudgetsNeverFail) {
	histogram h; // every budget defaults to 0 — disabled
	for (int i = 0; i < 10; ++i) h.record(1'000'000);

	EXPECT_TRUE(h.is_healthy());
}

TEST(MetricsHistogram, AnEmptyHistogramIsAlwaysHealthy) {
	const histogram h{latency_budgets{.p99_ns = 1, .p999_ns = 1, .max_ns = 1}};
	EXPECT_TRUE(h.is_healthy());
}

TEST(MetricsHistogram, ResetClearsEveryBucket) {
	histogram h;
	h.record(1);
	h.record(1'000'000);
	h.reset();

	const histogram::snapshot snap = h.read();
	EXPECT_EQ(snap.total, 0U);
	for (const auto count : snap.counts) EXPECT_EQ(count, 0U);
}

} // namespace
