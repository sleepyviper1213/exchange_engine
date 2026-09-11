#include "transport/rest/response.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cstddef>
#include <string>

namespace exchange::transport::rest {

namespace {

/// @brief Longest body worth putting on one log line.
///
/// A venue explains a refusal in a sentence and a proxy explains one in a web
/// page. Both arrive here, and the second is what an infrastructure error looks
/// like - an nginx 410 is eleven lines of HTML around three useful words.
constexpr std::size_t BODY_EXCERPT = 200;

/// @brief @p body, reduced to something that fits a log line.
///
/// Collapses whitespace and truncates, rather than parsing: what a *venue*
/// sends is JSON this tree already decodes elsewhere, and what reaches here
/// instead is by definition something nobody modelled. Guessing at its
/// structure would be the wrong response to not recognising it; keeping the
/// first line-and-a-bit is enough to tell an HTML error page from a JSON one
/// and to name the server that sent it.
[[nodiscard]] std::string summarised(const std::string &body) {
	std::string flat;
	flat.reserve(std::min(body.size(), BODY_EXCERPT + 1));
	bool gap = false;
	for (const char ch : body) {
		const bool space =
			ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
		if (space) {
			gap = !flat.empty();
			continue;
		}
		if (gap) flat.push_back(' ');
		gap = false;
		if (flat.size() >= BODY_EXCERPT) return flat + " ...";
		flat.push_back(ch);
	}
	return flat;
}

} // namespace

std::string failure::message() const {
	if (status == 0) return detail.empty() ? "request failed" : detail;
	if (retry_after)
		return fmt::format("HTTP {} (retry after {}s): {}",
						   status,
						   retry_after->count(),
						   body.empty() ? "no body" : body);
	// A trailing colon and nothing after it reads as a truncated message rather
	// than as what it is - a refusal the server declined to explain. Saying so
	// stops a reader looking for the half that was never sent.
	if (body.empty()) return fmt::format("HTTP {} (no body)", status);
	return fmt::format("HTTP {}: {}", status, summarised(body));
}

} // namespace exchange::transport::rest
