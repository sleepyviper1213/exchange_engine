#include "core/metrics/text_exposition.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>
#include <string_view>

using exchange::core::metrics::counter;
using exchange::core::metrics::histogram;
using exchange::core::metrics::registry;
using exchange::core::metrics::render_prometheus_text;

namespace {

TEST(MetricsTextExposition, AnEmptyRegistryRendersNothing) {
	const registry reg;
	EXPECT_EQ(render_prometheus_text(reg), "");
}

TEST(MetricsTextExposition, ACounterRendersItsTypeAndValue) {
	counter c;
	c.add(42);
	registry reg;
	reg.add("orders_processed", c);

	const std::string text = render_prometheus_text(reg);
	EXPECT_NE(text.find("# TYPE orders_processed counter"), std::string::npos);
	EXPECT_NE(text.find("orders_processed 42"), std::string::npos);
}

TEST(MetricsTextExposition, AHistogramRendersCumulativeBucketsAndACount) {
	histogram h;
	h.record(1);
	h.record(1);
	h.record(1000);
	registry reg;
	reg.add("latency_ns", h);

	const std::string text = render_prometheus_text(reg);
	EXPECT_NE(text.find("# TYPE latency_ns histogram"), std::string::npos);
	// bucket 1 ([1,1]) sees both fast observations before the slow one.
	EXPECT_NE(text.find("latency_ns_bucket{le=\"1\"} 2"), std::string::npos);
	EXPECT_NE(text.find("latency_ns_bucket{le=\"+Inf\"} 3"), std::string::npos);
	EXPECT_NE(text.find("latency_ns_count 3"), std::string::npos);
}

TEST(MetricsTextExposition, TheOutParamOverloadAppendsRatherThanOverwrites) {
	counter c;
	c.increment();
	registry reg;
	reg.add("x", c);

	fmt::memory_buffer out;
	fmt::format_to(fmt::appender(out), "prefix\n");
	render_prometheus_text(reg, out);

	const std::string_view text{out.data(), out.size()};
	EXPECT_EQ(text.rfind("prefix\n", 0), 0U);
	EXPECT_NE(text.find("x 1"), std::string_view::npos);
}

} // namespace
