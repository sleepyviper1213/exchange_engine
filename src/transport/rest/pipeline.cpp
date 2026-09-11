#include "transport/rest/pipeline.hpp"

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
using rest::detail::collect_headers;
using rest::detail::to_wire;
using transport::detail::TOKEN;

namespace {

/// A transport fault - no status, because there was no response.
[[nodiscard]] failure transport_failure(std::string why) {
	return failure{.detail = std::move(why)};
}

/// Fill @p out from @p at onward with @p why - the requests a broken batch
/// never got an answer for.
void fail_from(std::vector<response> &out, std::size_t at,
			   const std::string &why) {
	for (std::size_t i = at; i < out.size(); ++i)
		out[i] = std::unexpected(transport_failure(why));
}

} // namespace

/**
 * The connection and everything that happens over it.
 *
 * The helpers are members rather than free functions taking an @c impl& because
 * @c impl is a private nested type: a free function in this file's anonymous
 * namespace cannot name it, which is what stopped this translation unit
 * compiling for as long as it was absent from the build.
 */
struct request_pipeline::impl {
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

	/// What one write-then-read round produced.
	///
	/// @c answered is the count of requests the server actually replied to
	/// before the stream stopped being usable. Everything from that index on
	/// was never seen by the server, which is precisely the set that may be
	/// re-sent - the distinction the degrade path is built on, and the reason a
	/// batch bails at its first transport error rather than carrying on past
	/// one.
	struct batch_result {
		std::vector<response> out;
		std::size_t answered = 0;
	};

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

	/// Open and hand back a connected, handshaken stream, or say why not.
	asio::awaitable<std::expected<void, std::string>> connect();

	/// One write-then-read round over an established connection.
	asio::awaitable<batch_result> send_batch(std::span<const request> batch);
};

// --- connection ------------------------------------------------------------

asio::awaitable<std::expected<void, std::string>>
request_pipeline::impl::connect() {
	const auto executor = co_await asio::this_coro::executor;

	ctx.emplace(ssl::context::tls_client);
	// @see detail/trust_store.hpp
	if (options.verify == tls_verify::peer)
		(void)transport::detail::load_platform_trust_store(
			ctx->native_handle());
	ctx->set_verify_mode(options.verify == tls_verify::peer ? ssl::verify_peer
															: ssl::verify_none);
	stream.emplace(executor, *ctx);
	buffer.clear();

	if (SSL_set_tlsext_host_name(stream->native_handle(), host.c_str()) == 0) {
		drop();
		co_return std::unexpected("failed to set TLS SNI host name");
	}
	if (options.verify == tls_verify::peer)
		stream->set_verify_callback(ssl::host_name_verification(host));

	tcp::resolver resolver(executor);
	auto [resolve_ec, endpoints] =
		co_await resolver.async_resolve(host, "443", TOKEN);
	if (resolve_ec) {
		drop();
		co_return std::unexpected(
			fmt::format("resolve: {}", resolve_ec.message()));
	}

	beast::get_lowest_layer(*stream).expires_after(options.timeout);
	auto [connect_ec, endpoint] =
		co_await beast::get_lowest_layer(*stream).async_connect(endpoints,
																TOKEN);
	if (connect_ec) {
		drop();
		co_return std::unexpected(
			fmt::format("connect: {}", connect_ec.message()));
	}

	beast::get_lowest_layer(*stream).expires_after(options.timeout);
	if (auto [handshake_ec] =
			co_await stream->async_handshake(ssl::stream_base::client, TOKEN);
		handshake_ec) {
		drop();
		co_return std::unexpected(
			fmt::format("tls handshake: {}", handshake_ec.message()));
	}

	++stats.connections;
	co_return std::expected<void, std::string>{};
}

/**
 * Every request goes out before any reply is read - the whole point - and the
 * replies are then read in the order the requests went out, which HTTP/1.1
 * guarantees for a pipelined connection and which is the only thing matching a
 * reply to its request.
 */
asio::awaitable<request_pipeline::impl::batch_result>
request_pipeline::impl::send_batch(std::span<const request> batch) {
	batch_result result;
	result.out.assign(batch.size(),
					  std::unexpected(transport_failure("not sent")));

	// --- write the whole batch ---------------------------------------------
	for (const request &req : batch) {
		http::request<http::string_body> wire = to_wire(req, host);
		// Explicit, because it is the premise: without a kept-alive connection
		// there is no pipeline, only a sequence of conversations. The shared
		// builder leaves Connection alone precisely so this can say it.
		wire.keep_alive(true);

		beast::get_lowest_layer(*stream).expires_after(options.timeout);
		if (auto [write_ec, written] =
				co_await http::async_write(*stream, wire, TOKEN);
			write_ec) {
			// Nothing in this batch can be relied on now: the requests already
			// written may or may not have reached the server, but this one did
			// not, so their replies cannot be read back in a known order.
			fail_from(result.out,
					  0,
					  fmt::format("write: {}", write_ec.message()));
			drop();
			co_return result;
		}
	}

	// --- read the replies, in request order --------------------------------
	for (std::size_t i = 0; i < batch.size(); ++i) {
		http::response<http::string_body> res;
		beast::get_lowest_layer(*stream).expires_after(options.timeout);
		if (auto [read_ec, read] =
				co_await http::async_read(*stream, buffer, res, TOKEN);
			read_ec) {
			fail_from(result.out,
					  i,
					  fmt::format("read: {}", read_ec.message()));
			drop();
			co_return result;
		}

		const unsigned status       = res.result_int();
		std::vector<header> headers = collect_headers(res);
		std::string body            = std::move(res.body());
		result.out[i] =
			status == STATUS_OK
				? response{reply{.body    = std::move(body),
								 .headers = std::move(headers)}}
				: std::unexpected(failure{.status  = status,
										  .body    = std::move(body),
										  .headers = std::move(headers)});
		result.answered = i + 1;

		// The server is about to hang up. Its earlier replies stand; the
		// requests behind this one were never answered, so they go back for a
		// retry rather than being reported as refused. This is the ordinary way
		// an unpipelined server presents itself.
		if (!res.keep_alive()) {
			fail_from(result.out, i + 1, "server closed the connection");
			drop();
			co_return result;
		}
	}

	co_return result;
}

// --- the class -------------------------------------------------------------

request_pipeline::request_pipeline(std::string host, pipeline_options options)
	: impl_(std::in_place, std::move(host), options) {}

request_pipeline::~request_pipeline() {
	if (impl_.valueless_after_move()) return;
	impl_->drop();
}

request_pipeline::request_pipeline(request_pipeline &&) noexcept = default;
request_pipeline &
request_pipeline::operator=(request_pipeline &&) noexcept = default;

const pipeline_stats &request_pipeline::stats() const noexcept {
	return impl_->stats;
}

asio::awaitable<std::vector<response>>
request_pipeline::get(std::span<const std::string> targets) {
	std::vector<request> requests;
	requests.reserve(targets.size());
	for (const std::string &target : targets)
		requests.push_back(get_request(target));
	co_return co_await send(requests);
}

asio::awaitable<std::vector<response>>
request_pipeline::send(std::span<const request> requests) {
	impl &state = *impl_;
	std::vector<response> out;
	out.reserve(requests.size());
	state.stats.requests += requests.size();

	// 0 would send nothing at all, which is a configuration mistake rather than
	// a request to do nothing - read it as the smallest useful window.
	const std::size_t window = std::max<std::size_t>(1, state.options.window);

	std::size_t at = 0;
	while (at < requests.size()) {
		const std::size_t size = std::min(window, requests.size() - at);
		const std::span<const request> batch = requests.subspan(at, size);

		if (!state.is_connected())
			if (const auto opened = co_await state.connect(); !opened) {
				// Nothing can be sent at all; every remaining request reports
				// the same reason rather than the caller getting a short
				// vector.
				for (std::size_t i = at; i < requests.size(); ++i)
					out.emplace_back(
						std::unexpected(transport_failure(opened.error())));
				co_return out;
			}

		++state.stats.batches;
		impl::batch_result round = co_await state.send_batch(batch);

		// Everything the server answered is final, pipelined or not.
		for (std::size_t i = 0; i < round.answered; ++i)
			out.push_back(std::move(round.out[i]));

		const std::size_t unanswered = size - round.answered;
		if (unanswered == 0) {
			at += size;
			continue;
		}

		// The evidence says this connection will not carry a batch. Re-send
		// what it never saw, one request at a time, reconnecting as needed -
		// which is plain keep-alive, and is what the caller would have got from
		// window = 1 had they known to ask for it.
		//
		// Only where re-sending is safe. A POST that went out unanswered is not
		// retried here at any setting: the server may have acted on it, and a
		// duplicate order is worse than an unresolved one. Its slot keeps the
		// transport error, which is the caller's cue to go and reconcile.
		for (std::size_t i = round.answered; i < size; ++i) {
			if (!may_resend(batch[i], state.options.degrade_on_failure)) {
				out.push_back(std::move(round.out[i]));
				continue;
			}
			++state.stats.retried;
			if (!state.is_connected())
				if (const auto opened = co_await state.connect(); !opened) {
					for (std::size_t k = i; k < size; ++k)
						out.emplace_back(
							std::unexpected(transport_failure(opened.error())));
					co_return out;
				}
			++state.stats.batches;
			impl::batch_result single =
				co_await state.send_batch(batch.subspan(i, 1));
			out.push_back(std::move(single.out[0]));
		}
		at += size;
	}

	co_return out;
}

asio::awaitable<void> request_pipeline::close() {
	impl &state = *impl_;
	if (!state.is_connected()) co_return;
	// Best effort, exactly as https_get treats it: servers routinely close
	// without close_notify, and a stream_truncated on the way out is not
	// information anyone can act on.
	[[maybe_unused]] auto [ignored] =
		co_await state.stream->async_shutdown(TOKEN);
	state.drop();
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

std::vector<response> get_all(std::string host,
							  std::vector<std::string> targets,
							  pipeline_options options) {
	asio::io_context ioc;
	std::vector<response> result;
	const std::size_t expected = targets.size();

	asio::co_spawn(
		ioc,
		get_pipelined(std::move(host), std::move(targets), options),
		[&result](const std::exception_ptr &ep, std::vector<response> r) {
			if (ep) {
				try {
					std::rethrow_exception(ep);
				} catch (const std::exception &e) {
					result.clear();
					result.emplace_back(std::unexpected(transport_failure(
						fmt::format("exception: {}", e.what()))));
				}
			} else {
				result = std::move(r);
			}
		});
	ioc.run();

	// The one-response-per-target contract has to survive an exception too, so
	// a caller can keep indexing by target rather than checking the length.
	if (result.size() != expected) {
		const failure why = result.empty() || result.front().has_value()
								? transport_failure("pipeline did not run")
								: result.front().error();
		result.assign(expected, std::unexpected(why));
	}
	return result;
}

} // namespace exchange::transport::rest
