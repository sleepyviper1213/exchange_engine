#pragma once
#include <cstdint>
#include <type_traits>

#include "order.hpp"
#include "side.hpp"
#include "types.hpp"

namespace core::engine {

/// @brief Side/price/volume payload shared by ADD, REDUCE and SET_LEVEL.
struct LevelChange {
    Side side;
    Price price;
    Volume volume;
};

/**
 * @brief One unit of work handed to the matching engine over the SPSC queue: a
 *        tag plus the payload for exactly one book mutation.
 *
 * Deliberately a trivially copyable tagged union so @c spsc_queue<Command, N>
 * takes its @c memcpy batch path. There is no default constructor — build one
 * with a named factory (@c Command::place, @c Command::cancel, …) so the union's
 * active member always matches @c type.
 */
struct Command {
	/// @brief Which book mutation a Command carries.
	enum class Type : std::uint8_t {
		PLACE,     ///< place_order: cross, then rest the remainder
		CANCEL,    ///< cancel_order: remove a resting order by id
		ADD,       ///< add_order: rest anonymous liquidity, no matching
		REDUCE,    ///< delete_order: drain volume at a price, FIFO-first
		SET_LEVEL, ///< set_level: overwrite the absolute L2 size at a price
	};

    Type type;
    union {
        Order order;       ///< PLACE
        OrderId cancel_id; ///< CANCEL
        LevelChange level; ///< ADD / REDUCE / SET_LEVEL
    };

    [[nodiscard]] static Command place(const Order &o) noexcept {
        return Command(o);
    }
    [[nodiscard]] static Command cancel(OrderId id) noexcept {
        return Command(Type::CANCEL, id);
    }
    [[nodiscard]] static Command add(Side side, Price price, Volume volume) noexcept {
        return Command(Type::ADD, LevelChange{side, price, volume});
    }
    [[nodiscard]] static Command reduce(Side side, Price price, Volume volume) noexcept {
        return Command(Type::REDUCE, LevelChange{side, price, volume});
    }
    [[nodiscard]] static Command set_level(Side side, Price price, Volume volume) noexcept {
        return Command(Type::SET_LEVEL, LevelChange{side, price, volume});
    }

private:
    // Each ctor initialises exactly the union member that matches the tag, so
    // reading it back through the same tag is always the active member.
    explicit Command(const Order &o) noexcept
        : type(Type::PLACE), order(o) {}
    Command(Type t, OrderId id) noexcept : type(t), cancel_id(id) {}
    Command(Type t, LevelChange lc) noexcept : type(t), level(lc) {}
};

static_assert(std::is_trivially_copyable_v<Command>,
              "Command must stay trivially copyable for the queue's memcpy path");

} // namespace core::engine
