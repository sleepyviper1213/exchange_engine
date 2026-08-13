#include "core/metrics/format.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>

using exchange::core::metrics::counter;
using exchange::core::metrics::histogram;
using exchange::core::metrics::registry;

namespace {

TEST(RegistryFormat, RendersACounterAndAHistogramTogether) {
	counter c;
	c.add(42);
	histogram h;
	h.record(5); // bit_width(5) == 3 -> bucket 3, upper bound 7

	registry reg;
	reg.add("orders_processed", c);
	reg.add("latency_ns", h);

	const std::string text = fmt::format("{}", reg);
	EXPECT_TRUE(text.contains("# TYPE orders_processed counter\norders_processed 42\n"));
	EXPECT_TRUE(text.contains("# TYPE latency_ns histogram\n"));
	EXPECT_TRUE(text.contains("latency_ns_bucket{le=\"7\"} 1\n"));
	EXPECT_TRUE(text.contains("latency_ns_count 1\n"));
}

TEST(RegistryFormat, AnEmptyRegistryFormatsToNothing) {
	const registry reg;
	EXPECT_TRUE(fmt::format("{}", reg).empty());
}
} // namespace
