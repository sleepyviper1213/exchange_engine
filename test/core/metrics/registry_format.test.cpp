#include "core/metrics/format.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

using exchange::core::metrics::counter;
using exchange::core::metrics::histogram;
using exchange::core::metrics::registry;
using exchange::core::metrics::render_prometheus_text;

namespace {

TEST(RegistryFormat, MatchesRenderPrometheusTextVerbatim) {
	counter c;
	c.add(42);
	histogram h;
	h.record(5);

	registry reg;
	reg.add("orders_processed", c);
	reg.add("latency_ns", h);

	EXPECT_EQ(fmt::format("{}", reg), render_prometheus_text(reg));
}

TEST(RegistryFormat, AnEmptyRegistryFormatsToNothing) {
	const registry reg;
	EXPECT_EQ(fmt::format("{}", reg), "");
}

TEST(RegistryFormat, ComposesIntoALargerFormatCall) {
	counter c;
	c.increment();
	registry reg;
	reg.add("x", c);

	const std::string text = fmt::format("metrics dump:\n{}", reg);
	EXPECT_EQ(text, "metrics dump:\n# TYPE x counter\nx 1\n");
}

} // namespace
