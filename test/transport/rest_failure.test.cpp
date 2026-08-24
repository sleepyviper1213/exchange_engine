#include "transport/rest.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

// The retry policy, which is the only part of `transport::rest` that is a
// decision rather than I/O - and therefore the only part worth a unit suite.
//
// It matters more than its size suggests. Binance answers a rate-limit breach
// with 429 and answers a client that keeps sending after one with 418, an
// *address* ban that escalates from two minutes to three days. Whether the
// caller waits or retries is decided entirely by the predicates below, so a
// wrong answer here is not a cosmetic bug: it is how a client gets itself
// banned.

using namespace exchange::transport::rest;

TEST(TransportRestFailure, ARateLimitIsRetryableAndNamed) {
	const failure throttled{.status = STATUS_TOO_MANY_REQUESTS};

	EXPECT_TRUE(throttled.is_rate_limited());
	EXPECT_FALSE(throttled.is_ip_banned()) << "throttled, not banned - yet";
	EXPECT_TRUE(throttled.is_retryable()) << "later, and only later";
}

TEST(TransportRestFailure, ABanIsRecognisedAsWorseThanAThrottle) {
	const failure banned{.status = STATUS_IP_BANNED};

	EXPECT_TRUE(banned.is_rate_limited());
	EXPECT_TRUE(banned.is_ip_banned())
		<< "and a caller that cannot tell this from a 429 cannot know it is "
		   "already making things worse";
	EXPECT_TRUE(banned.is_retryable());
}

TEST(TransportRestFailure, ABadRequestIsNotWorthRetrying) {
	// The distinction the risk gate draws between back-pressure and a refusal,
	// applied here: an unknown symbol is refused identically for ever, and
	// retrying it spends rate-limit budget to learn nothing.
	const failure bad_symbol{.status        = 400,
							 .body          = R"({"code":-1121})"};

	EXPECT_FALSE(bad_symbol.is_rate_limited());
	EXPECT_FALSE(bad_symbol.is_retryable());
}

TEST(TransportRestFailure, AWafRefusalIsNotWorthRetryingEither) {
	EXPECT_FALSE(failure{.status = 403}.is_retryable())
		<< "403 is documented as the WAF, which will refuse the same request "
		   "the same way";
	EXPECT_FALSE(failure{.status = 404}.is_retryable());
}

TEST(TransportRestFailure, AServerErrorIsWorthRetrying) {
	// 5xx is the venue's problem rather than the request's, and Binance
	// documents it as an execution status that is *unknown* rather than failed -
	// so the request may in fact have been processed.
	EXPECT_TRUE(failure{.status = 500}.is_retryable());
	EXPECT_TRUE(failure{.status = 503}.is_retryable());
}

TEST(TransportRestFailure, NeverReachingTheVenueIsNotARefusal) {
	// Status zero is "we do not know whether the venue would have refused us",
	// which is a different thing from any 4xx and must not be collapsed into
	// one: a dropped link heals, and a caller that treated it as permanent
	// would stop trying.
	const failure unreachable{.status = 0, .detail = "connect: timed out"};

	EXPECT_FALSE(unreachable.is_rate_limited());
	EXPECT_TRUE(unreachable.is_retryable());
	EXPECT_EQ(unreachable.message(), "connect: timed out")
		<< "and the transport reason is what a log wants, not an invented status";
}

TEST(TransportRestFailure, AMessageIsProducedEvenWithNothingToSay) {
	EXPECT_FALSE(failure{}.message().empty())
		<< "a failure path that cannot describe itself is worse than the "
		   "failure it is reporting";
}

TEST(TransportRestFailure, TheRetryAfterIsCarriedIntoTheMessage) {
	const failure throttled{.status      = STATUS_TOO_MANY_REQUESTS,
							.body        = R"({"code":-1003})",
							.retry_after = std::chrono::seconds{30}};

	const std::string described = throttled.message();
	EXPECT_NE(described.find("429"), std::string::npos) << described;
	EXPECT_NE(described.find("30"), std::string::npos)
		<< "how long to wait is the actionable half: " << described;
}

TEST(TransportRestFailure, AnAbsentRetryAfterIsNotWaitZero) {
	// The distinction the caller's backoff depends on. `nullopt` means the
	// server did not say, and a caller must then use its own floor - retrying
	// immediately is exactly the behaviour that earns a 418.
	const failure throttled{.status = STATUS_TOO_MANY_REQUESTS};

	EXPECT_FALSE(throttled.retry_after.has_value());
	EXPECT_EQ(throttled.message().find("retry after"), std::string::npos)
		<< "and the message does not invent a number it was not given";
}
