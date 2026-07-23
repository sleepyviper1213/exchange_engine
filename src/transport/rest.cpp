#include "rest.hpp"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <fmt/format.h>

#include <chrono>
#include <exception>
namespace transport::rest {
asio::awaitable<std::expected<std::string, std::string>>
https_get(std::string host, std::string target) {
	const auto executor = co_await asio::this_coro::executor;

	ssl::context ctx(ssl::context::tls_client);
	ctx.set_default_verify_paths();
	// Public market data only: skip cert verification so we don't depend on a
	// CA bundle being installed. Do NOT do this for anything sensitive.
	ctx.set_verify_mode(ssl::verify_none);

	tcp::resolver resolver(executor);
	beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);

	// SNI — many hosts (incl. Binance) require it for the TLS handshake.
	if (SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()) == 0)
		co_return std::unexpected("failed to set TLS SNI host name");

	auto [resolve_ec, endpoints] =
		co_await resolver.async_resolve(host, "443", token);
	if (resolve_ec)
		co_return std::unexpected("resolve: " + resolve_ec.message());

	using namespace std::chrono_literals;
	beast::get_lowest_layer(stream).expires_after(10s);
	auto [connect_ec, connected_ep] =
		co_await beast::get_lowest_layer(stream).async_connect(endpoints,
															   token);
	if (connect_ec)
		co_return std::unexpected("connect: " + connect_ec.message());

	if (auto [handshake_ec] =
			co_await stream.async_handshake(ssl::stream_base::client, token);
		handshake_ec)
		co_return std::unexpected("tls handshake: " + handshake_ec.message());

	http::request<http::empty_body> req{http::verb::get, target, 11};
	req.set(http::field::host, host);
	req.set(http::field::user_agent, "order_book/1.0");
	req.set(http::field::accept, "application/json");

	beast::get_lowest_layer(stream).expires_after(10s);
	auto [write_ec, bytes_written] =
		co_await http::async_write(stream, req, token);
	if (write_ec) co_return std::unexpected("write: " + write_ec.message());

	beast::flat_buffer buffer;
	http::response<http::string_body> res;
	auto [read_ec, bytes_read] =
		co_await http::async_read(stream, buffer, res, token);
	if (read_ec) co_return std::unexpected("read: " + read_ec.message());

	std::string body      = std::move(res.body());
	const unsigned status = res.result_int();

	// Best-effort TLS shutdown; servers often close without close_notify
	// (stream_truncated), which is fine here.
	auto [_] = co_await stream.async_shutdown(token);

	if (status != 200)
		co_return std::unexpected(fmt::format("HTTP {}: {}", status, body));
	co_return body;
}

std::expected<std::string, std::string> get(std::string host,
											std::string target) {
	asio::io_context ioc;
	std::expected<std::string, std::string> result = std::unexpected("not run");
	asio::co_spawn(ioc,
				   https_get(std::move(host), std::move(target)),
				   [&result](std::exception_ptr ep,
							 std::expected<std::string, std::string>
								 r) {
					   if (ep) {
						   try {
							   std::rethrow_exception(ep);
						   } catch (const std::exception &e) {
							   result = std::unexpected(
								   std::string("exception: ") + e.what());
						   }
					   } else {
						   result = std::move(r);
					   }
				   });
	ioc.run();
	return result;
}
} // namespace transport::rest