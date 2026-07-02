/// @file
/// @brief AFL++ harness for the matching engine's identified-order flow.
///
/// Decodes the fuzzer bytes into a sequence of place_order/cancel_order calls
/// and checks the book's core invariants after every mutation. Only the
/// identified flow is driven here: add_order/delete_order/set_level rest or
/// overwrite liquidity without matching and can legitimately leave the book
/// crossed, so mixing them in would raise false positives — that L2 replay path
/// belongs in its own harness.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <fmt/format.h>

#include "afl_harness.hpp"
#include "engine.hpp"

namespace {

/// @brief Cursor over the fuzzer buffer; reads past the end yield 0 so a
///        truncated input simply stops issuing operations instead of reading OOB.
class Reader {
public:
    Reader(const std::uint8_t *data, std::size_t size) noexcept
        : data_(data), size_(size) {}

    [[nodiscard]] bool done() const noexcept { return pos_ >= size_; }

    std::uint8_t u8() noexcept {
        return pos_ < size_ ? data_[pos_++] : std::uint8_t{0};
    }

    std::uint64_t u64() noexcept {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | u8();
        return v;
    }

private:
    const std::uint8_t *data_;
    std::size_t size_;
    std::size_t pos_ = 0;
};

// Keep prices in a narrow band so incoming orders actually cross resting
// liquidity — a full 64-bit price space would almost never match and the
// matching path would go unexercised.
inline constexpr Price kPriceModulus = 64;
inline constexpr Volume kVolumeModulus = 1'000;
inline constexpr std::size_t kBookCapacity = 1u << 12;
inline constexpr std::uint8_t kCancelOp = 3; // 1-in-4 ops cancel; the rest place

[[noreturn]] void fail(std::string_view what) {
    fmt::print(stderr, "invariant violated: {}\n", what);
    std::abort(); // AFL++ records the abort as a crash
}

Side side_of(std::uint8_t b) noexcept {
    return (b & 1u) ? Side::ASK : Side::BID;
}
Price price_of(std::uint64_t v) noexcept {
    return static_cast<Price>(v % kPriceModulus) + 1;
}
Volume volume_of(std::uint64_t v) noexcept {
    return static_cast<Volume>(v % kVolumeModulus) + 1;
}
OrderType type_of(std::uint8_t b) noexcept {
    switch (b % 3u) {
    case 0:
        return OrderType::GOOD_TILL_CANCELED;
    case 1:
        return OrderType::FILL_OR_KILL;
    default:
        return OrderType::IMMEDIATE_OR_CANCEL;
    }
}

/// @brief After any mutation the sides must not be crossed: the best bid must
///        sit strictly below the best ask, else the matcher failed to execute
///        something it should have.
void check_invariants(const OrderBook &book) {
    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (bid && ask && *bid >= *ask) {
        fail(fmt::format("book crossed: best_bid={} >= best_ask={}", *bid, *ask));
    }
}

void run(const std::uint8_t *data, std::size_t size) {
    Reader in(data, size);
    OrderBook book(kBookCapacity);
    OrderId next_id = 1; // 0 is the engine's reserved anonymous id

    while (!in.done()) {
        const std::uint8_t op = in.u8();
        const std::uint8_t flags = in.u8();
        const std::uint64_t raw = in.u64();

        if (op % 4u == kCancelOp) {
            // Target an id we have actually issued (0..next_id-1). id 0 is the
            // reserved anonymous id and cancels to a documented no-op.
            book.cancel_order(raw % next_id);
        } else {
            book.place_order(Order{.id = next_id++,
                                   .side = side_of(flags),
                                   .price = price_of(raw),
                                   .volume = volume_of(raw),
                                   .type = type_of(static_cast<std::uint8_t>(flags >> 1))});
        }
        check_invariants(book);
    }
}

} // namespace

FUZZ_MAIN(run)
