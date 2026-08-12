#include "core/metrics/registry.hpp"

#include <gtest/gtest.h>

using exchange::core::metrics::counter;
using exchange::core::metrics::histogram;
using exchange::core::metrics::registry;

namespace {

TEST(MetricsRegistry, AFreshRegistryHasNoEntries) {
	const registry reg;
	EXPECT_TRUE(reg.entries().empty());
}

TEST(MetricsRegistry, AddNamesACounterByReference) {
	counter c;
	c.add(7);

	registry reg;
	reg.add("orders_processed", c);

	ASSERT_EQ(reg.entries().size(), 1U);
	const registry::entry &e = reg.entries()[0];
	EXPECT_EQ(e.name, "orders_processed");
	EXPECT_EQ(e.entry_kind, registry::kind::counter_metric);
	ASSERT_NE(e.as_counter, nullptr);
	EXPECT_EQ(e.as_counter->load(), 7U);
	EXPECT_EQ(e.as_histogram, nullptr);
}

TEST(MetricsRegistry, AddNamesAHistogramByReference) {
	histogram h;
	h.record(5);

	registry reg;
	reg.add("drain_latency_ns", h);

	ASSERT_EQ(reg.entries().size(), 1U);
	const registry::entry &e = reg.entries()[0];
	EXPECT_EQ(e.name, "drain_latency_ns");
	EXPECT_EQ(e.entry_kind, registry::kind::histogram_metric);
	ASSERT_NE(e.as_histogram, nullptr);
	EXPECT_EQ(e.as_histogram->read().total, 1U);
	EXPECT_EQ(e.as_counter, nullptr);
}

TEST(MetricsRegistry, EntriesPreserveRegistrationOrder) {
	counter a;
	counter b;
	histogram c;

	registry reg;
	reg.add("a", a);
	reg.add("b", b);
	reg.add("c", c);

	ASSERT_EQ(reg.entries().size(), 3U);
	EXPECT_EQ(reg.entries()[0].name, "a");
	EXPECT_EQ(reg.entries()[1].name, "b");
	EXPECT_EQ(reg.entries()[2].name, "c");
}

} // namespace
