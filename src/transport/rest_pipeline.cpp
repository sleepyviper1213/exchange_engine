#include "rest_pipeline.hpp"

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
#include <fmt/format.h>

#include <algorithm>
#include <exception>
#include <optional>
#include <utility>

namespace exchange::transport::rest {

namespace asio  = boost::asio;
namespace beast = boost::beast;
namespace http  = beast::http;
namespace ssl   = asio::ssl;
using tcp       = asio::ip::tcp;
using detail::kToken;

namespace {

/// What one write-then-read round produced.
///
/// @c answered is the count of targets the server actually replied to before
/// the stream stopped being usable. Everything from that index on was never
/// seen by the server, which is precisely the set that may be re-sent - the
/// distinction the degrade path is built on, and the reason a batch bails at
/// its first transport error rather than trying to carry on past one.
struct batch_result {
	std::vector<response> out;
	std::size_t answered = 0;
};

} // namespace

struct request_pipeline::Impl {
	std::string host;
	pipeline_options options;
	pipeline_stats stats;

	// Created on first use, from the executor the calling coroutine runs on,
	// and destroyed whenever the connection is dropped. optional rather than a
	// pointer because neither type is movable once bound to an executor.
	std::optional<ssl::context> ctx;
	std::optional<beast::ssl_stream<beast::tcp_stream>> stream;

	// Reused across every read on one connection, and that is not an
	// optimisation - it is required. A pipelined read routinely pulls bytes
	// belonging to the *next* response off the socket; they live here until
	// that response is parsed. A fresh buffer per read would discard them and
	// the second response of every batch would be garbage.
	beast::flat_buffer buffer;

	[[nodiscard]] bool is_connected() const noexcept {
		return stream.has_value();
	}

	/// Tear the connection down without a handshake-shutdown round trip. Used
	/// on every error path: the stream's state is unknown by then, so the
	/// polite close is not available and holding the socket open buys nothing.
	void drop() noexcept {
		if (stream) {
			beast::error_code ignored;
			(void)beast::get_lowest_layer(*stream).socket().close(ignored);
		}
		stream.reset();
		ctx.reset();
		buffer.clear();
	}
};

namespace {

/// Fill @p out from @p at onward with @p why - the requests a broken batch
/// never got an answer for.
void fail_from(std::vector<response> &out, std::size_t at,
			   const std::string &why) {
	for (std::size_t i = at; i < out.size(); ++i)
		out[i] = std::unexpected(why);
}

} // namespace

// --- connection ------------------------------------------------------------

namespace {

/// Open and hand back a connected, handshaken stream, or say why not.
asio::awaitable<std::expected<void, std::string>>
connect(request_pipeline::Impl &impl) {
	const auto executor = co_await asio::this_coro::executor;

	impl.ctx.emplace(ssl::context::tls_client);
	impl.ctx->set_default_verify_paths();
	// Public market data only - the same stance https_get takes, and for the
	// same reason: no dependency on a CA bundle being installed. Do NOT do this
	// for anything carrying a credential.
	impl.ctx->set_verify_mode(ssl::verify_none);
	impl.stream.emplace(executor, *impl.ctx);
	impl.buffer.clear();

	if (SSL_set_tlsext_host_name(impl.stream->native_handle(),
								 impl.host.c_str()) == 0) {
		impl.drop();
		co_return std::unexpected("failed to set TLS SNI host name");
	}

	tcp::resolver resolver(executor);
	auto [resolve_ec, endpoints] =
		co_await resolver.async_resolve(impl.host, "443", kToken);
	if (resolve_ec) {
		impl.drop();
		co_return std::unexpected(
			fmt::format("resolve: {}", resolve_ec.message()));
	}

	beast::get_lowest_layer(*impl.stream).expires_after(impl.options.timeout);
	auto [connect_ec, endpoint] =
		co_await beast::get_lowest_layer(*impl.stream)
			.async_connect(endpoints, kToken);
	if (connect_ec) {
		impl.drop();
		co_return std::unexpected(
			fmt::format("connect: {}", connect_ec.message()));
	}

	beast::get_lowest_layer(*impl.stream).expires_after(impl.options.timeout);
	if (auto [handshake_ec] = co_await impl.stream->async_handshake(
			ssl::stream_base::client, kToken);
		handshake_ec) {
		impl.drop();
		co_return std::unexpected(
			fmt::format("tls handshake: {}", handshake_ec.message()));
	}

	++impl.stats.connections;
	co_return std::expected<void, std::string>{};
}

/**
 * One write-then-read round over an established connection.
 *
 * Every request goes out before any reply is read - the whole point - and the
 * replies are then read in the order the requests went out, which HTTP/1.1
 * guarantees for a pipelined connection and which is the only thing matching a
 * reply to its request.
 */
asio::awaitable<batch_result> send_batch(request_pipeline::Impl &impl,
										 std::span<const std::string> targets) {
	batch_result result;
	result.out.assign(targets.size(), std::unexpected("not sent"));

	// --- write the whole batch ---------------------------------------------
	for (std::size_t i = 0; i < targets.size(); ++i) {
		http::request<http::empty_body> req{http::verb::get, targets[i], 11};
		req.set(http::field::host, impl.host);
		req.set(http::field::user_agent, "order_book/1.0");
		req.set(http::field::accept, "application/json");
		// Explicit, because it is the premise: without a kept-alive connection
		// there is no pipeline, only a sequence of conversations.
		req.keep_alive(true);

		beast::get_lowest_layer(*impl.stream)
			.expires_after(impl.options.timeout);
		if (auto [write_ec, written] =
				co_await http::async_write(*impl.stream, req, kToken);
			write_ec) {
			// Nothing in this batch can be relied on now: the requests already
			// written may or may not have reached the server, but this one did
			// not, so their replies cannot be read back in a known order.
			fail_from(result.out, 0,
					  fmt::format("write: {}", write_ec.message()));
			impl.drop();
			co_return result;
		}
	}

	// --- read the replies, in request order --------------------------------
	for (std::size_t i = 0; i < targets.size(); ++i) {
		http::response<http::string_body> res;
		beast::get_lowest_layer(*impl.stream)
			.expires_after(impl.options.timeout);
		if (auto [read_ec, read] =
				co_await http::async_read(*impl.stream, impl.buffer, res,
										  kToken);
			read_ec) {
			fail_from(result.out, i,
					  fmt::format("read: {}", read_ec.message()));
			impl.drop();
			co_return result;
		}

		const unsigned status = res.result_int();
		result.out[i] = status == 200
							? response{std::move(res.body())}
							: std::unexpected(fmt::format("HTTP {}: {}",
														  status,
														  res.body()));
		result.answered = i + 1;

		// The server is about to hang up. Its earlier replies stand; the
		// requests behind this one were never answered, so they go back for a
		// retry rather than being reported as refused. This is the ordinary way
		// an unpipelined server presents itself.
		if (!res.keep_alive()) {
			fail_from(result.out, i + 1, "server closed the connection");
			impl.drop();
			co_return result;
		}
	}

	co_return result;
}

} // namespace

// --- the class -------------------------------------------------------------

request_pipeline::request_pipeline(std::string host, pipeline_options options)
	: impl_(std::make_unique<Impl>(std::move(host), options)) {}

request_pipeline::~request_pipeline() {
	if (impl_) impl_->drop();
}

request_pipeline::request_pipeline(request_pipeline &&) noexcept = default;
request_pipeline &
request_pipeline::operator=(request_pipeline &&) noexcept = default;

const pipeline_stats &request_pipeline::stats() const noexcept {
	return impl_->stats;
}

asio::awaitable<std::vector<response>>
request_pipeline::get(std::span<const std::string> targets) {
	Impl &impl = *impl_;
	std::vector<response> out;
	out.reserve(targets.size());
	impl.stats.requests += targets.size();

	// 0 would send nothing at all, which is a configuration mistake rather than
	// a request to do nothing - read it as the smallest useful window.
	const std::size_t window = std::max<std::size_t>(1, impl.options.window);

	std::size_t at = 0;
	while (at < targets.size()) {
		const std::size_t size = std::min(window, targets.size() - at);
		const std::span<const std::string> batch = targets.subspan(at, size);

		if (!impl.is_connected())
			if (const auto opened = co_await connect(impl); !opened) {
				// Nothing can be sent at all; every remaining target reports the
				// same reason rather than the caller getting a short vector.
				for (std::size_t i = at; i < targets.size(); ++i)
					out.emplace_back(std::unexpected(opened.error()));
				co_return out;
			}

		++impl.stats.batches;
		batch_result round = co_await send_batch(impl, batch);

		// Everything the server answered is final, pipelined or not.
		for (std::size_t i = 0; i < round.answered; ++i)
			out.push_back(std::move(round.out[i]));

		const std::size_t unanswered = size - round.answered;
		if (unanswered == 0) {
			at += size;
			continue;
		}

		if (!impl.options.degrade_on_failure) {
			for (std::size_t i = round.answered; i < size; ++i)
				out.push_back(std::move(round.out[i]));
			at += size;
			continue;
		}

		// The evidence says this connection will not carry a batch. Re-send
		// what it never saw, one request at a time, reconnecting as needed -
		// which is plain keep-alive, and is what the caller would have got from
		// window = 1 had they known to ask for it.
		impl.stats.retried += unanswered;
		for (std::size_t i = round.answered; i < size; ++i) {
			if (!impl.is_connected())
				if (const auto opened = co_await connect(impl); !opened) {
					for (std::size_t k = i; k < size; ++k)
						out.emplace_back(std::unexpected(opened.error()));
					co_return out;
				}
			++impl.stats.batches;
			batch_result single = co_await send_batch(impl, batch.subspan(i, 1));
			out.push_back(std::move(single.out[0]));
		}
		at += size;
	}

	co_return out;
}

asio::awaitable<void> request_pipeline::close() {
	Impl &impl = *impl_;
	if (!impl.is_connected()) co_return;
	// Best effort, exactly as https_get treats it: servers routinely close
	// without close_notify, and a stream_truncated on the way out is not
	// information anyone can act on.
	auto [ignored] = co_await impl.stream->async_shutdown(kToken);
	impl.drop();
}

// --- free functions --------------------------------------------------------

asio::awaitable<std::vector<response>>
get_pipelined(std::string host, std::vector<std::string> targets,
			  pipeline_options options) {
	request_pipeline pipeline(std::move(host), options);
	std::vector<response> out = co_await pipeline.get(targets);
	co_await pipeline.close();
	co_return out;
}

std::vector<response> get_all(std::string host, std::vector<std::string> targets,
							  pipeline_options options) {
	asio::io_context ioc;
	std::vector<response> result;
	const std::size_t expected = targets.size();

	asio::co_spawn(
		ioc,
		get_pipelined(std::move(host), std::move(targets), options),
		[&result](std::exception_ptr ep, std::vector<response> r) {
			if (ep) {
				try {
					std::rethrow_exception(ep);
				} catch (const std::exception &e) {
					result.clear();
					result.emplace_back(
						std::unexpected(fmt::format("exception: {}", e.what())));
				}
			} else {
				result = std::move(r);
			}
		});
	ioc.run();

	// The one-response-per-target contract has to survive an exception too, so
	// a caller can keep indexing by target rather than checking the length.
	if (result.size() != expected) {
		const std::string why = result.empty()
									? std::string("pipeline did not run")
									: result.front().error_or(
										  std::string("pipeline failed"));
		result.assign(expected, std::unexpected(why));
	}
	return result;
}

} // namespace exchange::transport::rest
