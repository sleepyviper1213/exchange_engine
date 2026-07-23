#pragma once
#include <functional>
#include <iterator>
#include <ranges>

namespace optimisation {

/**
 * @brief Branchless equivalent of std::ranges::lower_bound.
 *
 * Finds the first position whose projected element does not satisfy
 * @c comp(element, value). The search advances by a bool-scaled stride instead
 * of an @c if, so the hot loop carries no data-dependent branch — a win for the
 * order book's small, cache-resident level vectors.
 */
struct branchless_lower_bound_fn {
    /**
     * @brief Iterator-pair overload.
     * @param first,last The sorted range to search.
     * @param value The value to position against.
     * @param comp Strict weak ordering (defaults to std::ranges::less).
     * @param proj Projection applied to each element before comparison.
     * @return Iterator to the first element not ordered before @p value.
     */
    template<
        std::random_access_iterator It,
        std::sized_sentinel_for<It> Sent,
        class T,
        class Proj = std::identity,
        std::indirect_strict_weak_order<
            const T *, std::projected<It, Proj> > Comp = std::ranges::less>
    constexpr It operator()(It first, Sent last, const T &value,
                            Comp comp = {}, Proj proj = {}) const {
        auto length = last - first;
        while (length > 0) {
            const auto half = length / 2;
            // Keep it branchless: bool result scales the advance, no `if`.
            const auto take = static_cast<std::iter_difference_t<It> >(
                static_cast<bool>(
                    std::invoke(comp, std::invoke(proj, first[half]), value)));
            first += take * (length - half);
            length = half;
        }
        return first;
    }

    template<
        std::ranges::random_access_range R,
        class T,
        class Proj = std::identity,
        std::indirect_strict_weak_order<
            const T *,
            std::projected<std::ranges::iterator_t<R>, Proj> > Comp = std::ranges::less>
    constexpr std::ranges::borrowed_iterator_t<R>
    operator()(R &&r, const T &value, Comp comp = {}, Proj proj = {}) const {
        return (*this)(std::ranges::begin(r), std::ranges::end(r), value,
                       std::move(comp), std::move(proj));
    }
};

inline constexpr branchless_lower_bound_fn branchless_lower_bound{};

} // namespace optimisation
