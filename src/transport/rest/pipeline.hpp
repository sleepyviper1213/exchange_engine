#pragma once

// Request pipelining over one HTTPS connection: many requests in flight, one
// handshake, responses matched by order.
//
// rest::https_get is a complete conversation per call - resolve, connect, TLS
// handshake, write, read, shut down. That is the right shape for one request
// and the wrong shape for twenty: fetching a depth snapshot for each listing in
// a deployment pays twenty handshakes and twenty sequential round trips, and
// the twenty are almost entirely spent waiting.
//
// This is the same trick Redis documents as pipelining. Issue the next request
// without waiting for the previous reply; the replies come back in the order
// the requests went out, so they need no identifier to be matched to them.
// @see https://redis.io/docs/latest/develop/using-commands/pipelining/
//
//   N requests, window W:   1 handshake + ceil(N/W) round trips
//   the same through https_get:   N handshakes + N round trips
//
// --- why a batch, and not a sliding window ---------------------------------
//
// The obvious refinement - keep exactly W in flight by writing one more each
// time a reply lands - is not available on a TLS stream. A bare TCP socket
// supports a concurrent read and write; an SSL stream does not, because both
// directions run through one non-thread-safe SSL object. So a write cannot be
// issued while a read is pending, and the achievable shape is the batch:
//
//   write r1 r2 ... rW      (awaited in turn, but no reply waited for)
//   read  s1 s2 ... sW      (in the order the requests went out)
//
// Which is exactly what a Redis client does, for a different reason. The cost
// is head-of-line blocking within a batch - one slow response delays the
// replies queued behind it - and the mitigation is a smaller window, not a
// cleverer loop.
//
// --- the hazard --------------------------------------------------------
//
// HTTP/1.1 pipelining is legal and poorly supported. Some servers answer the
// first request and close; proxies mangle it more creatively. That is why
// `window = 1` is a supported setting rather than a degenerate one - it still
// reuses the connection, so it still saves every handshake but the first - and
// why the pipeline retries unanswered requests one at a time rather than
// failing the batch. A server that will not pipeline is detected from its
// behaviour instead of configured in advance.

#include "core/util/indirect.hpp"
#include "transport/rest/options.hpp"  // IWYU pragma: export
#include "transport/rest/request.hpp"  // IWYU pragma: export
#include "transport/rest/response.hpp" // IWYU pragma: export
#include "transport_export.hpp"        // TRANSPORT_EXPORT (generated)

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace exchange::transport::rest {

/// @brief How a @ref request_pipeline behaves.
struct pipeline_options {
	/**
	 * @brief Requests written before any reply is read.
	 *
	 * The round-trip divisor: @c N requests cost @c ceil(N/window) of them.
	 * Also the head-of-line blocking window, since a batch is only as quick as
	 * its slowest member. 0 is read as 1.
	 *
	 * @c 1 disables pipelining while keeping the connection, which is the
	 * setting for a server known to mishandle it - and still worth having,
	 * because the handshake is the expensive half.
	 */
	std::size_t window = 8;

	/// @brief Per-operation deadline on the underlying socket.
	std::chrono::seconds timeout{10};

	/**
	 * @brief Retry unanswered requests one at a time on a fresh connection.
	 *
	 * A server that answers the first request of a batch and closes is not
	 * broken, it is unpipelined - and the requests behind that close were never
	 * refused, they were never seen. Retrying them singly turns the worst case
	 * back into plain keep-alive rather than into an error the caller has to
	 * know how to interpret.
	 *
	 * @note This is permission, not instruction. A request is re-sent only if
	 *       this is on @b and its method is idempotent, so a batch carrying a
	 *       POST cannot resend it however this is set - @see may_resend. The
	 *       gate is per request rather than per pipeline because a mixed batch
	 *       is the ordinary case the moment a caller both reads and writes.
	 */
	bool degrade_on_failure = true;

	/**
	 * @brief Certificate policy for this connection. @see tls_verify
	 *
	 * Defaults to unverified, matching @c https_get rather than
	 * @c https_request: this class was written for market-data fan-out and
	 * every existing caller wants that. A pipeline carrying a credential must
	 * set @c tls_verify::peer.
	 */
	tls_verify verify = tls_verify::none;
};

/// @brief What a pipeline has done - the evidence for the round-trip claim.
struct pipeline_stats {
	std::uint64_t requests    = 0; ///< Targets asked for.
	std::uint64_t batches     = 0; ///< Write-then-read rounds - round trips.
	std::uint64_t connections = 0; ///< TLS handshakes paid.
	std::uint64_t retried     = 0; ///< Requests re-sent after an unanswered
								   ///< batch. @see degrade_on_failure
};

/**
 * @brief A persistent HTTPS connection that issues requests without waiting
 *        for each reply.
 *
 * @code
 * request_pipeline pipe("api.binance.com");
 * const auto bodies = co_await pipe.get(targets);   // one handshake, N/W trips
 * @endcode
 *
 * The connection is opened on first use and kept for the pipeline's lifetime,
 * so a long-lived caller - a reconstructor that demands a fresh snapshot
 * whenever its replica dies - pays the handshake once for the session rather
 * than once per demand.
 *
 * @par Ordering is the correlation
 * There is no request id in HTTP/1.1; a pipelined server must answer in the
 * order it was asked. So the i-th response in the returned vector is the answer
 * to the i-th target, and that is a property of the protocol rather than
 * something this class tracks.
 *
 * @note Not thread-safe, and single-conversation by construction: one
 *       connection has one request order, so two callers sharing a pipeline
 *       would interleave their batches into one stream and read each other's
 *       replies. One pipeline per consuming coroutine.
 */
class request_pipeline {
public:
	/**
	 * @brief A pipeline against @p host. Nothing is opened until the first
	 *        request.
	 * @param host TLS host (e.g. @c api.binance.com); also the SNI and the
	 *        @c Host header.
	 * @param options Window, deadline and degrade policy.
	 */
	TRANSPORT_EXPORT explicit request_pipeline(std::string host,
											   pipeline_options options = {});
	TRANSPORT_EXPORT ~request_pipeline();
	/**
	 * @brief Not copyable, and with @c core::util::indirect that is now a
	 *        decision rather than a consequence.
	 *
	 * A @c unique_ptr pimpl cannot be copied, so the deletion used to be the
	 * language's doing. @c indirect *is* copyable - deep-copying what it owns
	 * is the whole reason C++26 adds it - so a copyable request_pipeline is
	 * only a
	 * `= default` away, and it is refused because a second pipeline sharing one
	 * connection is not a copy of anything - the SSL stream and its read buffer
	 * are bound to one socket and one executor.
	 */
	request_pipeline(const request_pipeline &)            = delete;
	request_pipeline &operator=(const request_pipeline &) = delete;

	/**
	 * @brief Movable, and both halves defined out of line.
	 *
	 * @c impl is incomplete here, and moving out of an @c indirect steals a
	 * pointer while move-*assignment* first destroys what this one owns - which
	 * needs the complete type. So does the destructor. All three are declared
	 * here and defined in the .cpp beside @c impl; a compiler-generated one in
	 * this header would not compile.
	 *
	 * @post The moved-from object owns no @c impl. @see
	 * indirect::valueless_after_move
	 */
	TRANSPORT_EXPORT request_pipeline(request_pipeline &&) noexcept;
	TRANSPORT_EXPORT request_pipeline &operator=(request_pipeline &&) noexcept;

	/**
	 * @brief Fetch every target, pipelined, over this connection.
	 *
	 * @param targets Request paths with query, e.g.
	 *        @c /api/v3/depth?symbol=SOLUSDT&limit=100. Borrowed for the call.
	 * @return One response per target, in the same order. Always the same
	 *         length as @p targets - a request that could not be issued reports
	 *         why in its own slot rather than shortening the result, so the
	 *         positional correspondence a caller relies on holds even for a
	 *         batch that went wrong.
	 *
	 * @note An HTTP status other than 200 is a response, not a failure of the
	 *       pipeline: it is reported in that slot and the connection carries
	 * on. Only a transport fault drops the connection, and only the requests
	 *       that were never answered are retried. @see degrade_on_failure
	 */
	[[nodiscard]] TRANSPORT_EXPORT boost::asio::awaitable<std::vector<response>>
	get(std::span<const std::string> targets);

	/**
	 * @brief Send every request, pipelined, over this connection.
	 *
	 * The general form @ref get is written in terms of. Same ordering contract,
	 * same one-slot-per-input guarantee.
	 *
	 * @param requests What to send, in order. Borrowed for the call.
	 * @return One response per request, in the same order.
	 *
	 * @note A request that was written and never answered is only re-sent if
	 *       @c may_resend allows it. One that is not says so in its slot and
	 *       stays unresolved on purpose: a POST in that state may or may not
	 *       have been acted on by the server, and only the caller knows how to
	 *       find out. Do not paper over it with a retry.
	 */
	[[nodiscard]] TRANSPORT_EXPORT boost::asio::awaitable<std::vector<response>>
	send(std::span<const request> requests);

	/// @brief Close the connection gracefully; the next send reopens it.
	TRANSPORT_EXPORT boost::asio::awaitable<void> close();

	/// @brief Counters since construction. @see pipeline_stats
	[[nodiscard]] TRANSPORT_EXPORT const pipeline_stats &stats() const noexcept;

private:
	struct impl;
	core::util::indirect<impl> impl_;
};

/**
 * @brief Fetch every target from @p host over one pipelined connection.
 *
 * The one-shot form: opens, pipelines the lot, closes. Use @ref
 * request_pipeline directly when the connection should outlive the batch.
 * @param host TLS host.
 * @param targets Request paths with query.
 * @param options Window, deadline and degrade policy.
 * @return One response per target, in order.
 */
[[nodiscard]] TRANSPORT_EXPORT boost::asio::awaitable<std::vector<response>>
get_pipelined(std::string host, std::vector<std::string> targets,
			  pipeline_options options = {});

/**
 * @brief Blocking wrapper around @ref get_pipelined: spins up a local
 *        io_context and runs the whole batch to completion.
 * @param host TLS host.
 * @param targets Request paths with query.
 * @param options Window, deadline and degrade policy.
 * @return One response per target, in order.
 */
[[nodiscard]] TRANSPORT_EXPORT std::vector<response>
get_all(std::string host, std::vector<std::string> targets,
		pipeline_options options = {});

} // namespace exchange::transport::rest
