#include "core/simd/target.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>

using namespace exchange;
using namespace exchange::core;

namespace {

// Not an assertion about the machine - CI runs on whatever it runs on - but a
// record of which path the rest of the suite actually exercised. A ladder test
// that passes proves nothing about AVX-512 if the dispatch resolved to SSE2,
// and without this line there is no way to tell the two runs apart afterwards.
TEST(SimdTarget, ReportsTheDispatchedInstructionSet) {
	const std::string_view target = simd::active_target();
	EXPECT_FALSE(target.empty());
	GTEST_LOG_(INFO) << "core::simd dispatched to " << target << ", "
					 << simd::register_bytes() << "-byte vectors";
}

// A vector register is a power of two bytes wide and at least as wide as the
// widest lane the kernels use. A zero or absurd value here means the dispatch
// table was never initialised, which would otherwise show up as a crash
// somewhere unrelated.
TEST(SimdTarget, RegisterWidthIsAPlausiblePowerOfTwo) {
	const std::size_t bytes = simd::register_bytes();
	ASSERT_GE(bytes, sizeof(long long));
	EXPECT_EQ(bytes & (bytes - 1), 0u) << bytes << " is not a power of two";
}

} // namespace
