#include "rest.hpp"

#include "detail/coroutine_token.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <fmt/format.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string_view>

namespace exchange::transport::rest {

// Boost namespace aliases are kept private to this translation unit so the
// public rest.hpp no longer leaks them into every includer.
namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace ssl   = asio::ssl;
using tcp       = asio::ip::tcp;
using detail::kToken;

namespace {

/**
 * @brief Seconds from a @c Retry-After header, or nothing.
 *
 * Only the delta-seconds form is read. RFC 9110 also allows an HTTP-date, and a
 * date is deliberately *not* parsed here: it would have to be compared against a
 * clock, and a client whose clock is wrong would then compute a negative wait
 * and hammer the endpoint it had just been asked to leave alone. Binance sends
 * seconds. An unparseable value is treated as absent, which falls back to the
 * caller's own backoff - slower than the server asked for, never faster.
 */
[[nodiscard]] std::optional<std::chrono::seconds>
parse_retry_after(std::string_view header) noexcept {
	if (header.empty()) return std::nullopt;
	std::uint64_t seconds = 0;
	const auto *const end = header.data() + header.size();
	const auto [stop, ec] = std::from_chars(header.data(), end, seconds);
	if (ec != std::errc{} || stop != end) return std::nullopt;
	return std::chrono::seconds{static_cast<std::chrono::seconds::rep>(seconds)};
}

} // namespace

std::string failure::message() const {
	if (status == 0) return detail.empty() ? "request failed" : detail;
	if (retry_after)
		return fmt::format("HTTP {} (retry after {}s): {}",
						   status,
						   retry_after->count(),
						   body);
	return fmt::format("HTTP {}: {}", status, body);
}

asio::awaitable<std::expected<std::string, failure>>
https_get(std::string host, std::string target) {
	const auto executor = co_await asio::this_coro::executor;

	ssl::context ctx(ssl::context::tls_client);
	ctx.set_default_verify_paths();
	// Public market data only: skip cert verification so we don't depend on a
	// CA bundle being installed. Do NOT do this for anything sensitive.
	ctx.set_verify_mode(ssl::verify_none);

	tcp::resolver resolver(executor);
	beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);

	// SNI - many hosts (incl. Binance) require it for the TLS handshake.
	if (SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) == 0)
		co_return std::unexpected(
			failure{.detail = "failed to set TLS SNI host name"});

	auto [resolve_ec, endpoints] =
		co_await resolver.async_resolve(host, "443", kToken);
	if (resolve_ec)
		co_return std::unexpected(
			failure{.detail = fmt::format("resolve: {}", resolve_ec.message())});

	using namespace std::chrono_literals;
	beast::get_lowest_layer(stream).expires_after(10s);
	auto [connect_ec, connected_ep] =
		co_await beast::get_lowest_layer(stream).async_connect(endpoints,
															   kToken);
	if (connect_ec)
		co_return std::unexpected(
			failure{.detail = fmt::format("connect: {}", connect_ec.message())});

	if (auto [handshake_ec] =
			co_await stream.async_handshake(ssl::stream_base::client, kToken);
		handshake_ec)
		co_return std::unexpected(failure{
			.detail = fmt::format("tls handshake: {}", handshake_ec.message())});

	http::request<http::empty_body> req{http::verb::get, target, 11};
	req.set(http::field::host, host);
	req.set(http::field::user_agent, "order_book/1.0");
	req.set(http::field::accept, "application/json");

	beast::get_lowest_layer(stream).expires_after(10s);
	auto [write_ec, bytes_written] =
		co_await http::async_write(stream, req, kToken);
	if (write_ec)
		co_return std::unexpected(
			failure{.detail = fmt::format("write: {}", write_ec.message())});

	beast::flat_buffer buffer;
	http::response<http::string_body> res;
	auto [read_ec, bytes_read] =
		co_await http::async_read(stream, buffer, res, kToken);
	if (read_ec)
		co_return std::unexpected(
			failure{.detail = fmt::format("read: {}", read_ec.message())});

	std::string body      = std::move(res.body());
	const unsigned status = res.result_int();
	// Read before the shutdown below, because `res` is what owns the header
	// storage the view would point into.
	const std::optional<std::chrono::seconds> retry_after =
		parse_retry_after(res[http::field::retry_after]);

	// Best-effort TLS shutdown; servers often close without close_notify
	// (stream_truncated), which is fine here.
	auto [_] = co_await stream.async_shutdown(kToken);

	if (status != STATUS_OK)
		co_return std::unexpected(failure{.status      = status,
										  .body        = std::move(body),
										  .retry_after = retry_after});
	co_return body;
}

std::expected<std::string, failure> get(std::string host, std::string target) {
	asio::io_context ioc;
	std::expected<std::string, failure> result =
		std::unexpected(failure{.detail = "not run"});
	asio::co_spawn(ioc,
				   https_get(std::move(host), std::move(target)),
				   [&result](std::exception_ptr ep,
							 std::expected<std::string, failure> r) {
					   if (ep) {
						   try {
							   std::rethrow_exception(ep);
						   } catch (const std::exception &e) {
							   result = std::unexpected(failure{
								   .detail =
									   fmt::format("exception: {}", e.what())});
						   }
					   } else {
						   result = std::move(r);
					   }
				   });
	ioc.run();
	return result;
}
} // namespace exchange::transport::rest
