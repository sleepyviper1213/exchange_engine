#pragma once
// Private, shared completion token for the module's Boost.Asio coroutines. Not
// part of the public API — include only from within the transport module. It
// lives here (rather than in each public header) so that rest.hpp/websocket.hpp
// stay minimal and do not leak Asio detail into every includer.
#include <boost/asio/as_tuple.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace exchange::transport::detail {

/**
 * @brief The @c co_await completion token shared by @c rest and @c websocket.
 *
 * @c as_tuple delivers each completion as a tuple led by the @c error_code, so
 * failures stay values — no exception is thrown across a @c co_await and each
 * step unpacks its own result with a structured binding. Both transports use
 * the identical token so their error handling reads the same way.
 */
inline constexpr auto kToken =
	boost::asio::as_tuple(boost::asio::use_awaitable);

} // namespace exchange::transport::detail
