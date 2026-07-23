#pragma once

namespace order_book {

/// @brief Book side of an order. Encoded as a bool for branchless opposed().
enum class Side : bool {
    BID, ASK
};

/**
 * @brief The opposite side of @p s (BID <-> ASK).
 * @param s A book side.
 * @return The opposing side.
 */
constexpr Side opposed(Side s) {
    return static_cast<Side>(!static_cast<bool>(s));
}

} // namespace order_book
