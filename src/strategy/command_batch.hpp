#pragma once
#include "command_writer.hpp"
#include "core/util/start_lifetime_as.hpp"

#include <array>

namespace exchange::strategy {

/**
 * @brief Inline storage for @p Capacity commands, plus the cursor over it.
 *
 * No allocation, ever: the bytes are a member, and the capacity comes from a
 * compile-time sum over the strategies the host carries. A batch is filled
 * across several events and handed to the sink whole, so the queue sees one
 * @c try_emplace_range rather than one @c try_emplace per command.
 *
 * @tparam Capacity Number of commands the buffer holds. Must be at least one.
 *
 * @note Neither copyable nor movable, and cannot become either: @c writer_
 * holds pointers into @c storage_, so any relocation would leave a cursor aimed
 *       at the corpse. Same reasoning as @c execution::engine_partition - the
 *       object is pinned to the thread that drains its host, and there is
 *       nowhere for one to move to.
 */
template <std::size_t Capacity>
class command_batch {
	static_assert(Capacity > 0, "a batch with no room cannot accept a command");

public:
	/// @brief Storage for @p symbol's commands, empty and ready to write.
	explicit command_batch(symbol_id_t symbol) noexcept
		: writer_(core::util::start_lifetime_as_array<engine::event::command>(
					  storage_.data(), Capacity),
				  Capacity, symbol) {}

	command_batch(const command_batch &)            = delete;
	command_batch &operator=(const command_batch &) = delete;
	command_batch(command_batch &&)                 = delete;
	command_batch &operator=(command_batch &&)      = delete;
	~command_batch()                                = default;

	/// @brief The cursor. Hand this to a strategy.
	[[nodiscard]] command_writer &writer() noexcept EXCHANGE_LIFETIMEBOUND {
		return writer_;
	}

	[[nodiscard]] const command_writer &
	writer() const noexcept EXCHANGE_LIFETIMEBOUND {
		return writer_;
	}

	/// @brief What has accumulated since the last @c writer().reset().
	[[nodiscard]] std::span<const engine::event::command>
	view() const noexcept EXCHANGE_LIFETIMEBOUND {
		return writer_.written();
	}

	[[nodiscard]] std::size_t size() const noexcept { return writer_.size(); }

	static constexpr std::size_t CAPACITY = Capacity;

private:
	// storage_ is declared first because writer_ points into it, and members
	// initialise in declaration order.
	//
	// Bytes rather than std::array<command, Capacity>: command has no default
	// constructor by design - a named factory picks the union's active member -
	// so an array of them cannot be default-initialised.
	// start_lifetime_as_array begins the lifetimes of trivially copyable
	// objects over the bytes without constructing anything, which is exactly
	// the missing step, and leaves a genuine array so a span over it is
	// well-formed.
	alignas(engine::event::command) std::array<
		std::byte, Capacity * sizeof(engine::event::command)> storage_;
	command_writer writer_;
};
} // namespace exchange::strategy