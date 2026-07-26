#include "core/optimisation/branchless_binary_search.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <vector>

using namespace exchange::core::optimisation;

TEST(BranchlessLowerBound, EmptyRangeReturnsEnd) {
    std::vector<int> v;
    EXPECT_EQ(branchless_lower_bound(v, 42), v.end());
}

TEST(BranchlessLowerBound, ValueBelowAllReturnsBegin) {
    std::vector<int> v{10, 20, 30};
    EXPECT_EQ(branchless_lower_bound(v, 5), v.begin());
}

TEST(BranchlessLowerBound, ValueAboveAllReturnsEnd) {
    std::vector<int> v{10, 20, 30};
    EXPECT_EQ(branchless_lower_bound(v, 99), v.end());
}

TEST(BranchlessLowerBound, ExactMatchReturnsThatElement) {
    std::vector<int> v{10, 20, 30, 40};
    const auto it = branchless_lower_bound(v, 30);
    ASSERT_NE(it, v.end());
    EXPECT_EQ(*it, 30);
    EXPECT_EQ(it - v.begin(), 2);
}

TEST(BranchlessLowerBound, NoMatchReturnsInsertionPoint) {
    std::vector<int> v{10, 20, 30, 40};
    const auto it = branchless_lower_bound(v, 25);
    ASSERT_NE(it, v.end());
    EXPECT_EQ(*it, 30); // first element not less than 25
}

TEST(BranchlessLowerBound, MatchesStdLowerBound) {
    const std::vector<int> v{1, 3, 3, 3, 5, 8, 13, 21};
    for (int value = 0; value <= 22; ++value) {
        const auto expected = std::ranges::lower_bound(v, value);
        const auto actual = branchless_lower_bound(v, value);
        EXPECT_EQ(actual - v.begin(),
                  expected - v.begin()) << "value=" << value;
    }
}

TEST(BranchlessLowerBound, WorksWithGreaterForDescendingRange) {
    // Bid side is kept descending; lower_bound with std::greater finds the
    // first element that is <= value (the insertion point for a new level).
    std::vector<int> v{105, 103, 100, 98};
    const auto it = branchless_lower_bound(v, 103, std::greater<int>{});
    ASSERT_NE(it, v.end());
    EXPECT_EQ(*it, 103);
}