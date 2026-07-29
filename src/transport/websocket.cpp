#include "websocket.hpp"

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
#include <boost/beast/websocket.hpp>
#include <fmt/std.h>

#include <fstream>

namespace exchange::transport::ws {

// Boost namespace aliases are kept private to this translation unit so the
// public websocket.hpp no longer leaks them into every includer.
namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace ssl       = asio::ssl;
using tcp           = asio::ip::tcp;
using detail::kToken;

asio::awaitable<std::expected<void, std::string>>
capture_to_file(std::string host, std::string port, std::string target,
				std::string outfile, std::chrono::seconds duration) {
	using namespace std::chrono_literals;
	const auto executor = co_await asio::this_coro::executor;

	ssl::context ctx(ssl::context::tls_client);
	ctx.set_default_verify_paths();
	// Public market data only; skip cert verification so we don't depend on a
	// CA bundle. Do NOT copy this for anything sensitive.
	ctx.set_verify_mode(ssl::verify_none);

	tcp::resolver resolver(executor);
	websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(executor, ctx);

	// SNI — Binance requires it for the TLS handshake.
	if (SSL_set_tlsext_host_name(ws.next_layer().native_handle(),
								 host.c_str()) == 0)
		co_return std::unexpected("failed to set TLS SNI host name");

	auto [resolve_ec, endpoints] =
		co_await resolver.async_resolve(host, port, kToken);
	if (resolve_ec)
		co_return std::unexpected(fmt::format("resolve: {}", resolve_ec.message()));

	beast::get_lowest_layer(ws).expires_after(10s);
	auto [connect_ec, endpoint] =
		co_await beast::get_lowest_layer(ws).async_connect(endpoints, kToken);
	if (connect_ec)
		co_return std::unexpected(fmt::format("connect: {}", connect_ec.message()));

	if (auto [handshake_ec] =
			co_await ws.next_layer().async_handshake(ssl::stream_base::client,
													 kToken);
		handshake_ec)
		co_return std::unexpected(
			fmt::format("tls handshake: {}", handshake_ec.message()));

	// Hand timeout management to the websocket layer (ping keepalive + idle
	// timeout); the raw tcp deadline must be cleared or it fights the ws
	// stream.
	beast::get_lowest_layer(ws).expires_never();
	ws.set_option(
		websocket::stream_base::timeout::suggested(beast::role_type::client));
	ws.set_option(
		websocket::stream_base::decorator([](websocket::request_type &req) {
			req.set(http::field::user_agent, "order_book/1.0");
		}));

	// RFC 6455 Host header carries the port for the ws upgrade.
	const std::string host_header = fmt::format("{}:{}", host, port);
	if (auto [ws_ec] = co_await ws.async_handshake(host_header, target, kToken);
		ws_ec)
		co_return std::unexpected(fmt::format("ws handshake: {}", ws_ec.message()));

	std::ofstream out(outfile, std::ios::binary | std::ios::trunc);
	if (!out) co_return std::unexpected(
			fmt::format("cannot open output file: {}", outfile));

	const auto deadline  = std::chrono::steady_clock::now() + duration;
	std::uint64_t frames = 0;
	beast::flat_buffer buffer;
	while (std::chrono::steady_clock::now() < deadline) {
		auto [read_ec, bytes] = co_await ws.async_read(buffer, kToken);
		if (read_ec) {
			if (read_ec == websocket::error::closed) break; // server closed
			co_return std::unexpected(fmt::format("read: {}", read_ec.message()));
		}
		const auto line = beast::buffers_to_string(buffer.data());
		out.write(line.data(), static_cast<std::streamsize>(line.size()));
		out.put('\n');
		buffer.consume(buffer.size());
		if (++frames % 50 == 0) fmt::print(stderr, "\rframes: {}", frames);
	}
	out.flush();

	// Best-effort graceful close; a truncated close from the server is fine.
	auto _ = co_await ws.async_close(websocket::close_code::normal, kToken);

	fmt::println(stderr, "\ncaptured {} frames to {}", frames, outfile);
	co_return std::expected<void, std::string>{};
}

std::expected<void, std::string> capture(std::string host, std::string port,
										 std::string target,
										 std::string outfile,
										 std::chrono::seconds duration) {
	asio::io_context ioc;
	std::expected<void, std::string> result = std::unexpected("not run");
	asio::co_spawn(ioc,
				   capture_to_file(std::move(host),
								   std::move(port),
								   std::move(target),
								   std::move(outfile),
								   duration),
				   [&result](const std::exception_ptr &ep,
							 std::expected<void, std::string>
								 r) {
					   if (ep) {
						   try {
							   std::rethrow_exception(ep);
						   } catch (const std::exception &e) {
							   result = std::unexpected(
								   fmt::format("exception: {}", e.what()));
						   }
					   } else {
						   result = std::move(r);
					   }
				   });
	ioc.run();
	return result;
}
} // namespace exchange::transport::ws