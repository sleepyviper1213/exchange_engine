#include "websocket.hpp"

#include "detail/coroutine_token.hpp"
#include "detail/trust_store.hpp"

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
#include <optional>
#include <utility>

namespace exchange::transport::ws {

// Boost namespace aliases are kept private to this translation unit so the
// public websocket.hpp no longer leaks them into every includer.
namespace asio      = boost::asio;
namespace beast     = boost::beast;
namespace http      = beast::http;
namespace websocket = beast::websocket;
namespace ssl       = asio::ssl;
using tcp           = asio::ip::tcp;
using detail::TOKEN;

struct stream_reader::impl {
	std::string host;
	std::string port;
	std::string target;
	tls_verify verify      = tls_verify::peer;
	std::uint64_t frames   = 0;
	std::uint64_t connects = 0;

	// Both are built in connect() from the calling coroutine's executor and
	// destroyed on close, so a reader can be reconnected without being
	// reconstructed. optional rather than a pointer because neither is movable
	// once bound to an executor.
	std::optional<ssl::context> ctx;
	std::optional<websocket::stream<beast::ssl_stream<beast::tcp_stream>>> ws;

	/// The frame the last read() handed out a view of. Owned here so the view
	/// stays valid exactly until the next read, which is the documented
	/// contract.
	beast::flat_buffer buffer;

	void drop() noexcept {
		ws.reset();
		ctx.reset();
		buffer.clear();
	}
};

stream_reader::stream_reader(std::string host, std::string port,
							 std::string target, tls_verify verify)
	: impl_(std::in_place) {
	impl_->host   = std::move(host);
	impl_->port   = std::move(port);
	impl_->target = std::move(target);
	impl_->verify = verify;
}

stream_reader::~stream_reader() {
	if (impl_.valueless_after_move()) return;
	impl_->drop();
}

stream_reader::stream_reader(stream_reader &&) noexcept            = default;
stream_reader &stream_reader::operator=(stream_reader &&) noexcept = default;

// The three observers all tolerate a moved-from reader, which owns no impl at
// all - moving one leaves impl_ valueless, and the destructor has always
// checked for that. These did not, so the only defined thing to do with a
// moved-from reader was destroy it, and asking it anything was undefined
// behaviour. A moved-from stream is closed, has carried no frames and has made
// no connections, which is both true and the answer a caller would want.

bool stream_reader::is_open() const noexcept {
	return !impl_.valueless_after_move() && impl_->ws.has_value();
}

std::uint64_t stream_reader::frames() const noexcept {
	return !impl_.valueless_after_move() ? impl_->frames : 0;
}

std::uint64_t stream_reader::connects() const noexcept {
	return !impl_.valueless_after_move() ? impl_->connects : 0;
}

asio::awaitable<std::expected<void, std::string>> stream_reader::connect() {
	using namespace std::chrono_literals;
	if (impl_.valueless_after_move())
		co_return std::unexpected(
			std::string("reader was moved from and owns no stream"));

	impl &state         = *impl_;
	const auto executor = co_await asio::this_coro::executor;

	state.drop();
	state.ctx.emplace(ssl::context::tls_client);
	// Not set_default_verify_paths: on Windows that points OpenSSL at an
	// OPENSSLDIR which does not exist, leaving an empty store that fails every
	// verify_peer handshake with "certificate verify failed" - naming the venue
	// for what is really a missing CA bundle. @see detail/trust_store.hpp
	if (state.verify == tls_verify::peer) {
		const auto trust =
			detail::load_platform_trust_store(state.ctx->native_handle());
		if (trust.certificates == 0) {
			state.drop();
			co_return std::unexpected(fmt::format(
				"no TLS trust store available ({}), so the feed's certificate "
				"cannot be verified; install a CA bundle or pass "
				"--insecure-tls to read the feed unverified",
				trust.source));
		}
	}
	state.ctx->set_verify_mode(
		state.verify == tls_verify::peer ? ssl::verify_peer : ssl::verify_none);
	state.ws.emplace(executor, *state.ctx);
	auto &stream = *state.ws;

	// SNI - Binance requires it for the TLS handshake. It is also what
	// verify_peer matches the certificate's name against, so setting it is part
	// of the verification and not only of the routing. Same three-part setup as
	// rest::https_request: trust store, verify mode, host-name callback.
	if (SSL_set_tlsext_host_name(stream.next_layer().native_handle(),
								 state.host.c_str()) == 0) {
		state.drop();
		co_return std::unexpected("failed to set TLS SNI host name");
	}

	// A chain that verifies but belongs to somebody else is not a connection to
	// the venue, so the name check goes on alongside the mode - one without the
	// other is not verification.
	if (state.verify == tls_verify::peer)
		stream.next_layer().set_verify_callback(
			ssl::host_name_verification(state.host));

	tcp::resolver resolver(executor);
	auto [resolve_ec, endpoints] =
		co_await resolver.async_resolve(state.host, state.port, TOKEN);
	if (resolve_ec) {
		state.drop();
		co_return std::unexpected(
			fmt::format("resolve: {}", resolve_ec.message()));
	}

	beast::get_lowest_layer(stream).expires_after(10s);
	auto [connect_ec, endpoint] =
		co_await beast::get_lowest_layer(stream).async_connect(endpoints,
															   TOKEN);
	if (connect_ec) {
		state.drop();
		co_return std::unexpected(
			fmt::format("connect: {}", connect_ec.message()));
	}

	if (auto [handshake_ec] = co_await stream.next_layer().async_handshake(
			ssl::stream_base::client,
			TOKEN);
		handshake_ec) {
		state.drop();
		co_return std::unexpected(
			fmt::format("tls handshake: {}", handshake_ec.message()));
	}

	// Hand timeout management to the websocket layer (ping keepalive + idle
	// timeout); the raw tcp deadline must be cleared or it fights the ws
	// stream.
	beast::get_lowest_layer(stream).expires_never();
	stream.set_option(
		websocket::stream_base::timeout::suggested(beast::role_type::client));
	stream.set_option(
		websocket::stream_base::decorator([](websocket::request_type &req) {
			req.set(http::field::user_agent, "order_book/1.0");
		}));

	// RFC 6455 Host header carries the port for the ws upgrade.
	const std::string host_header =
		fmt::format("{}:{}", state.host, state.port);
	if (auto [ws_ec] =
			co_await stream.async_handshake(host_header, state.target, TOKEN);
		ws_ec) {
		state.drop();
		co_return std::unexpected(
			fmt::format("ws handshake: {}", ws_ec.message()));
	}

	++state.connects;
	co_return std::expected<void, std::string>{};
}

asio::awaitable<std::expected<std::string_view, stream_status>>
stream_reader::read() {
	// A moved-from reader answers the same way an unconnected one does: it owns
	// no stream, which is the thing the caller needs to know either way.
	if (impl_.valueless_after_move() || !impl_->ws.has_value())
		co_return std::unexpected(
			stream_status{stream_stop::failed, "stream is not connected"});

	impl &state = *impl_;

	// The previous frame's bytes are released here rather than after handing
	// the view out, which is what makes "valid until the next read" true.
	state.buffer.consume(state.buffer.size());

	auto [read_ec, bytes] = co_await state.ws->async_read(state.buffer, TOKEN);
	if (read_ec) {
		const bool closed = read_ec == websocket::error::closed;
		state.drop();
		co_return std::unexpected(
			stream_status{closed ? stream_stop::closed : stream_stop::failed,
						  closed ? std::string{} : read_ec.message()});
	}

	++state.frames;
	const auto data = state.buffer.data();
	co_return std::string_view(static_cast<const char *>(data.data()),
							   data.size());
}

asio::awaitable<std::expected<void, std::string>>
stream_reader::send(std::string_view text) {
	if (impl_.valueless_after_move() || !impl_->ws.has_value())
		co_return std::unexpected(std::string("stream is not connected"));

	impl &state = *impl_;
	// Text rather than binary, because every venue request this carries is
	// JSON. Set per write instead of once at connect: the flag is stream state,
	// and a reader that also captured binary frames would have to set it back.
	state.ws->text(true);

	auto [write_ec, bytes] =
		co_await state.ws->async_write(asio::buffer(text), TOKEN);
	if (write_ec) {
		// Dropped for the same reason a failed read drops: a half-written frame
		// leaves the peer's parser mid-message, so the connection is no longer
		// one anybody can reason about. The caller reconnects.
		state.drop();
		co_return std::unexpected(write_ec.message());
	}
	(void)bytes;
	co_return std::expected<void, std::string>{};
}

asio::awaitable<void> stream_reader::close() {
	if (impl_.valueless_after_move()) co_return;
	impl &state = *impl_;
	if (!state.ws.has_value()) co_return;
	// Best-effort graceful close; a truncated close from the server is fine.
	[[maybe_unused]] auto [ignored] =
		co_await state.ws->async_close(websocket::close_code::normal, TOKEN);
	state.drop();
}

// --- capture ---------------------------------------------------------------
//
// Written on top of stream_reader rather than beside it: the resolve, connect,
// TLS and upgrade sequence is the same sequence either way, and it existed
// twice for exactly as long as it took to need it twice.

asio::awaitable<std::expected<void, std::string>>
capture_to_file(std::string host, std::string port, std::string target,
				std::string outfile, std::chrono::seconds duration,
				tls_verify verify) {
	stream_reader reader(std::move(host),
						 std::move(port),
						 std::move(target),
						 verify);
	if (const auto opened = co_await reader.connect(); !opened)
		co_return std::unexpected(opened.error());

	std::ofstream out(outfile, std::ios::binary | std::ios::trunc);
	if (!out) {
		co_await reader.close();
		co_return std::unexpected(
			fmt::format("cannot open output file: {}", outfile));
	}

	const auto deadline = std::chrono::steady_clock::now() + duration;
	while (std::chrono::steady_clock::now() < deadline) {
		const auto frame = co_await reader.read();
		if (!frame) {
			if (is_closed(frame.error())) break; // server closed
			co_return std::unexpected(
				fmt::format("read: {}", frame.error().detail));
		}
		out.write(frame->data(), static_cast<std::streamsize>(frame->size()));
		out.put('\n');
		if (reader.frames() % 50 == 0)
			fmt::print(stderr, "\rframes: {}", reader.frames());
	}
	out.flush();

	co_await reader.close();
	fmt::println(stderr,
				 "\ncaptured {} frames to {}",
				 reader.frames(),
				 outfile);
	co_return std::expected<void, std::string>{};
}

std::expected<void, std::string>
capture(std::string host, std::string port, std::string target,
		std::string outfile, std::chrono::seconds duration, tls_verify verify) {
	asio::io_context ioc;
	std::expected<void, std::string> result = std::unexpected("not run");
	asio::co_spawn(ioc,
				   capture_to_file(std::move(host),
								   std::move(port),
								   std::move(target),
								   std::move(outfile),
								   duration,
								   verify),
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
