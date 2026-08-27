#include "core/persistence/format.hpp"

#include <fmt/format.h>
#include <gtest/gtest.h>

#include <string>

// The rendering is meant to be read beside the file it names, so what these
// pin is that agreement: the same three keys, in the order `save` writes them,
// on one line - plus the nested_formatter padding contract every other record
// in this project honours.

using exchange::core::persistence::manifest;

namespace {

TEST(ManifestFormat, PrintsTheThreeNumbersUnderTheFilesOwnKeys) {
	const manifest current{.snapshot_id = 7, .sequence = 1024, .session = 3};
	EXPECT_EQ(fmt::format("{}", current),
			  "manifest[snapshot_id=7 sequence=1024 session=3]");
}

TEST(ManifestFormat, AZeroedManifestPrintsZeroForEveryField) {
	// Zero is a meaningful value for all three - `load` keeps a bit per field
	// precisely so a missing key is not read as one - so it must render as a
	// number rather than being elided.
	EXPECT_EQ(fmt::format("{}", manifest{}),
			  "manifest[snapshot_id=0 sequence=0 session=0]");
}

TEST(ManifestFormat, WidthAndAlignmentApplyToTheWholeRecord) {
	const manifest current{.snapshot_id = 7, .sequence = 1024, .session = 3};
	const std::string unpadded = fmt::format("{}", current);
	const std::string padded   = fmt::format("{:>60}", current);

	// nested_formatter's parse() consumes the width, so a correct format() pads
	// the record rather than swallowing the spec.
	EXPECT_EQ(padded.size(), 60U);
	EXPECT_TRUE(padded.ends_with(unpadded));
	EXPECT_EQ(fmt::format("{:<60}", current),
			  unpadded + std::string(60 - unpadded.size(), ' '));
}

TEST(ManifestFormat, WidthNarrowerThanTheRecordDoesNotTruncate) {
	const manifest current{.snapshot_id = 7, .sequence = 1024, .session = 3};
	EXPECT_EQ(fmt::format("{:>4}", current), fmt::format("{}", current));
}

} // namespace
