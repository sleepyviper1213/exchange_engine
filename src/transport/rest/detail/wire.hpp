#pragma once
// Turning this module's `request` into Beast's, in one place.
//
// Private to the rest submodule - include only from its sources, and never from
// a public header, because this is what keeps Beast out of `request.hpp`.
//
// One definition, shared by both senders: `client.cpp` and `pipeline.cpp` build
// the same wire request from the same value, and a copy each is a redefinition
// the moment `ORDER_BOOK_ENABLE_UNITY_BUILD` concatenates them into one
// translation unit - anonymous namespaces merge along with everything else in a
// batch. Inline in a named namespace is the shape that survives it.

#include "transport/rest/request.hpp"

#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace exchange::transport::rest::detail {

/// @brief What this client calls itself, on every request it sends.
inline constexpr std::string_view USER_AGENT = "order_book/1.0";

/// @brief HTTP/1.1, as Beast's integer version.
inline constexpr int HTTP_1_1 = 11;

/**
 * @brief @p m as Beast spells it.
 *
 * @note Exhaustive with no @c default, so a verb added to @c method fails to
 *       compile here rather than silently going out as a GET. The return after
 *       the switch is unreachable for any valid enumerator and exists only
 *       because the parameter's type does not constrain its value.
 */
[[nodiscard]] inline constexpr boost::beast::http::verb
beast_verb(method m) noexcept {
	namespace http = boost::beast::http;
	switch (m) {
	case method::get: return http::verb::get;
	case method::post: return http::verb::post;
	case method::put: return http::verb::put;
	case method::del: return http::verb::delete_;
	}
	return http::verb::get;
}

/**
 * @brief @p req as a wire message addressed to @p host.
 *
 * Sets the headers the sender owns - @c Host, @c User-Agent, @c Accept, and
 * @c Content-Type / @c Content-Length when there is a body - then whatever the
 * request named for itself.
 *
 * @note @c prepare_payload is called unconditionally: a POST with no body still
 *       needs @c Content-Length: @c 0, or a server is entitled to wait for one
 *       that never comes.
 * @note Does not touch @c Connection. The pipeline requires keep-alive and says
 *       so itself; the one-shot client has no opinion and lets the default
 *       stand, which is the behaviour each had before they shared this.
 */
[[nodiscard]] inline boost::beast::http::request<
	boost::beast::http::string_body>
to_wire(const request &req, std::string_view host) {
	namespace http = boost::beast::http;

	http::request<http::string_body> wire{beast_verb(req.verb),
										  req.target,
										  HTTP_1_1};
	wire.set(http::field::host, host);
	wire.set(http::field::user_agent, USER_AGENT);
	wire.set(http::field::accept, "application/json");
	for (const header &h : req.headers) wire.set(h.name, h.value);
	if (!req.body.empty()) {
		wire.set(http::field::content_type, req.content_type);
		wire.body() = req.body;
	}
	wire.prepare_payload();
	return wire;
}

/**
 * @brief Every header @p res carried, copied out of Beast's storage.
 *
 * Copied rather than viewed because the message that owns the field storage is
 * a local of whoever read it: a span of views into it would dangle the moment
 * the response went out of scope, which is one line later in both senders.
 *
 * @note Whole, not filtered. Which headers matter is the venue's question, and
 *       transport does not know the venue - @see reply::headers.
 */
[[nodiscard]] inline std::vector<header> collect_headers(
	const boost::beast::http::response<boost::beast::http::string_body> &res) {
	std::vector<header> out;
	out.reserve(static_cast<std::size_t>(std::ranges::distance(res)));
	for (const auto &field : res)
		out.push_back(header{.name  = std::string(field.name_string()),
							 .value = std::string(field.value())});
	return out;
}

} // namespace exchange::transport::rest::detail
