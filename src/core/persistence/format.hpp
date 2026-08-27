#pragma once
// The recovery pointer, as text.
// Opt-in, like every other <module>/format.hpp here: only a translation unit
// that actually prints a manifest pays for <fmt/format.h>, so manifest.hpp
// stays free of it. Include this wherever you format one; a missing include is
// a compile error, never a silently different rendering.

#include "manifest.hpp"

#include <fmt/base.h>
#include <fmt/format.h>

#include <string_view>

/**
 * @brief A manifest as
 *        @c "manifest[snapshot_id=7 sequence=1024 session=3]".
 *
 * @par Why it borrows the file's own key names and field order
 * Because the two are read together and almost always in that order: a recovery
 * logs the manifest it is resuming from, and the next thing anyone does when
 * that looks wrong is @c cat the file. Printing @c snapshot=7 here and
 * @c snapshot_id=7 there would make a grep that finds one miss the other, for
 * no gain - so the rendering is the file's three lines on one line, in the
 * order @c save writes them.
 *
 * That is a deliberate duplication of the spellings @c manifest.cpp declares
 * once for the format itself, and it is the cheap direction of the coupling:
 * the file is the authority, this is a diagnostic, and the two drifting apart
 * costs a confusing log line rather than an unreadable manifest. The
 * alternative - hoisting the keys into the public header so both read one
 * constant - would put the file format's vocabulary in the API to save a
 * three-word comment.
 *
 * @note Derived from @c fmt::nested_formatter like every other record in this
 *       project, so fill, align and width apply to the whole record rather than
 *       being swallowed: @c {:>48} right-aligns a manifest in a log column.
 */
template <>
struct fmt::formatter<exchange::core::persistence::manifest>
	: fmt::nested_formatter<std::string_view> {
	auto format(const exchange::core::persistence::manifest &current,
				format_context &ctx) const -> format_context::iterator {
		return write_padded(ctx, [&](auto out) {
			return fmt::format_to(
				out,
				"manifest[snapshot_id={} sequence={} session={}]",
				current.snapshot_id,
				current.sequence,
				current.session);
		});
	}
};
