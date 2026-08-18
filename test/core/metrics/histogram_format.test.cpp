#include "core/metrics/format.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>

using exchange::core::metrics::histogram;

namespace {

TEST(HistogramFormat, AnEmptySnapshotPrintsZeroForEveryField) {
	const histogram h;
	EXPECT_EQ(fmt::format("{}", h.read()),
			  "histogram[n=0 p50=0ns p99=0ns p999=0ns]");
}

TEST(HistogramFormat, PrintsCountAndPercentilesFromTheSnapshot) {
	histogram h;
	for (int i = 0; i < 90; ++i) h.record(1);    // bucket 1: [1,1]
	for (int i = 0; i < 10; ++i) h.record(1000); // bucket 10: [512,1023]

	EXPECT_EQ(fmt::format("{}", h.read()),
			  "histogram[n=100 p50=1ns p99=1023ns p999=1023ns]");
}

TEST(HistogramFormat, WidthAndAlignmentApplyToTheWholeRecord) {
	const histogram h;
	const std::string unpadded = fmt::format("{}", h.read());
	const std::string padded   = fmt::format("{:>40}", h.read());

	// nested_formatter's parse() consumes the width, so a correct format()
	// pads the whole record rather than swallowing the spec - the contract
	// core/concurrency/affinity/format.hpp's own test pins the same way.
	EXPECT_EQ(padded.size(), 40U);
	EXPECT_TRUE(padded.ends_with(unpadded));
}

} // namespace
