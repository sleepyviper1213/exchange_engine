#include "transport/rest/client.hpp"

#include "transport/detail/coroutine_token.hpp"
#include "transport/detail/trust_store.hpp"
#include "transport/rest/detail/wire.hpp"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <fmt/format.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <string_view>
#include <utility>

namespace exchange::transport::rest {

// Boost namespace aliases are kept private to this translation unit so the
// public headers no longer leak them into every includer.
namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace ssl   = asio::ssl;
using tcp       = asio::ip::tcp;
using rest::detail::collect_headers;
using rest::detail::to_wire;
using transport::detail::TOKEN;

namespace {

/**
 * @brief Seconds from a @c Retry-After header, or nothing.
 *
 * Only the delta-seconds form is read. RFC 9110 also allows an HTTP-date, and a
 * date is deliberately *not* parsed here: it would have to be compared against
 * a clock, and a client whose clock is wrong would then compute a negative wait
 * and hammer the endpoint it had just been asked to leave alone. Binance sends
 * seconds. An unparseable value is treated as absent, which falls back to the
 * caller's own backoff - slower than the server asked for, never faster.
 */
[[nodiscard]] std::optional<std::chrono::seconds>
parse_retry_after(std::string_view value) noexcept {
	if (value.empty()) return std::nullopt;
	std::uint64_t seconds = 0;
	const auto *const end = value.data() + value.size();
	const auto [stop, ec] = std::from_chars(value.data(), end, seconds);
	if (ec != std::errc{} || stop != end) return std::nullopt;
	return std::chrono::seconds{
		static_cast<std::chrono::seconds::rep>(seconds)};
}

/**
 * A handshake diagnosis that names the fix.
 *
 * `certificate verify failed` is the one TLS error an operator can act on and
 * the one they cannot act on from the message alone - the cause is almost
 * always that OpenSSL found no trust store, which on Windows is the default
 * rather than a misconfiguration. Saying so here costs nothing and saves the
 * hour that error otherwise takes.
 */
[[nodiscard]] std::string describe_handshake(const beast::error_code &ec,
											 tls_verify verify) {
	if (verify == tls_verify::peer)
		return fmt::format(
			"tls handshake: {} (certificate verification is on; if this says "
			"the chain could not be verified, point OpenSSL at a CA bundle "
			"with SSL_CERT_FILE or SSL_CERT_DIR)",
			ec.message());
	return fmt::format("tls handshake: {}", ec.message());
}

/// One request over an already-connected stream, and the response it produced.
asio::awaitable<response> converse(beast::ssl_stream<beast::tcp_stream> &stream,
								   std::string_view host, const request &req,
								   std::chrono::seconds timeout) {
	http::request<http::string_body> wire = to_wire(req, host);

	beast::get_lowest_layer(stream).expires_after(timeout);
	if (auto [write_ec, written] =
			co_await http::async_write(stream, wire, TOKEN);
		write_ec)
		co_return std::unexpected(
			failure{.detail = fmt::format("write: {}", write_ec.message())});

	beast::flat_buffer buffer;
	http::response<http::string_body> res;
	beast::get_lowest_layer(stream).expires_after(timeout);
	if (auto [read_ec, read] =
			co_await http::async_read(stream, buffer, res, TOKEN);
		read_ec)
		co_return std::unexpected(
			failure{.detail = fmt::format("read: {}", read_ec.message())});

	const unsigned status = res.result_int();
	// Both read before the body is moved out, because `res` is what owns the
	// storage the views point into.
	const std::optional<std::chrono::seconds> retry_after =
		parse_retry_after(res[http::field::retry_after]);
	std::vector<header> headers = collect_headers(res);
	std::string body            = std::move(res.body());

	if (status != STATUS_OK)
		co_return std::unexpected(failure{.status      = status,
										  .body        = std::move(body),
										  .retry_after = retry_after,
										  .headers     = std::move(headers)});
	co_return reply{.body = std::move(body), .headers = std::move(headers)};
}

/// Run @p work on a private io_context and hand back what it returned, turning
/// an escaped exception into a failure rather than a terminate.
template <typename Awaitable>
[[nodiscard]] response block_on(Awaitable work) {
	asio::io_context ioc;
	response result = std::unexpected(failure{.detail = "not run"});
	asio::co_spawn(
		ioc,
		std::move(work),
		[&result](const std::exception_ptr &ep, response r) {
			if (ep) {
				try {
					std::rethrow_exception(ep);
				} catch (const std::exception &e) {
					result = std::unexpected(failure{
						.detail = fmt::format("exception: {}", e.what())});
				}
			} else {
				result = std::move(r);
			}
		});
	ioc.run();
	return result;
}

} // namespace

asio::awaitable<response> https_request(std::string host, request req,
										request_options options) {
	const auto executor = co_await asio::this_coro::executor;

	ssl::context ctx(ssl::context::tls_client);
	// @see detail/trust_store.hpp - set_default_verify_paths finds nothing on
	// Windows, which is a failed handshake rather than a warning.
	if (options.verify == tls_verify::peer) {
		const auto trust =
			transport::detail::load_platform_trust_store(ctx.native_handle());
		if (trust.certificates == 0)
			co_return std::unexpected(failure{
				.detail = fmt::format("no TLS trust store available ({}), so "
									  "the server's certificate cannot be "
									  "verified",
									  trust.source)});
	}
	ctx.set_verify_mode(options.verify == tls_verify::peer ? ssl::verify_peer
														   : ssl::verify_none);

	tcp::resolver resolver(executor);
	beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);

	// SNI - many hosts (incl. Binance) require it for the TLS handshake. It is
	// also what verify_peer matches the certificate's name against, so setting
	// it is part of the verification, not only of the routing.
	if (SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) == 0)
		co_return std::unexpected(
			failure{.detail = "failed to set TLS SNI host name"});
	if (options.verify == tls_verify::peer)
		stream.set_verify_callback(ssl::host_name_verification(host));

	auto [resolve_ec, endpoints] =
		co_await resolver.async_resolve(host, "443", TOKEN);
	if (resolve_ec)
		co_return std::unexpected(failure{
			.detail = fmt::format("resolve: {}", resolve_ec.message())});

	beast::get_lowest_layer(stream).expires_after(options.timeout);
	auto [connect_ec, connected_ep] =
		co_await beast::get_lowest_layer(stream).async_connect(endpoints,
															   TOKEN);
	if (connect_ec)
		co_return std::unexpected(failure{
			.detail = fmt::format("connect: {}", connect_ec.message())});

	beast::get_lowest_layer(stream).expires_after(options.timeout);
	if (auto [handshake_ec] =
			co_await stream.async_handshake(ssl::stream_base::client, TOKEN);
		handshake_ec)
		co_return std::unexpected(failure{
			.detail = describe_handshake(handshake_ec, options.verify)});

	response result = co_await converse(stream, host, req, options.timeout);

	// Best-effort TLS shutdown; servers often close without close_notify
	// (stream_truncated), which is fine here.
	[[maybe_unused]] auto [ignored] = co_await stream.async_shutdown(TOKEN);

	co_return result;
}

asio::awaitable<response> https_get(std::string host, std::string target) {
	// Unverified on purpose, and documented as such in the header: this is the
	// public-market-data path, which must keep working without a CA bundle.
	co_return co_await https_request(
		std::move(host),
		get_request(std::move(target)),
		request_options{.verify  = tls_verify::none,
						.timeout = std::chrono::seconds{10}});
}

response send(std::string host, request req, request_options options) {
	return block_on(https_request(std::move(host), std::move(req), options));
}

response get(std::string host, std::string target) {
	return block_on(https_get(std::move(host), std::move(target)));
}

} // namespace exchange::transport::rest
