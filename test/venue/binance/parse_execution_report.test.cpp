#include "venue/binance/user_data.hpp"

#include <gtest/gtest.h>

#include <string>

// Decoding what the venue says happened to an order we sent.
//
// The cases that matter are the ones where two fields disagree in a way a naive
// reader would miss: a cancel of a partly-filled order still reports
// PARTIALLY_FILLED, and a fill can arrive naming an order whose acknowledgement
// has not been seen. Getting either wrong double-counts a fill or retires the
// wrong order.

using exchange::venue::execution_kind;
using exchange::venue::execution_status;
using exchange::venue::binance::parse_execution_report;
using exchange::venue::binance::user_data_error;

namespace {

/// SOLUSDT's real grid: two decimals of price, three of size.
constexpr int REPORT_PRICE_DECIMALS = 2;
constexpr int REPORT_QTY_DECIMALS   = 3;

/// A resting buy the venue has just accepted; nothing traded.
constexpr auto REPORT_ACK = R"({
  "e":"executionReport","E":1499405658658,"s":"SOLUSDT","c":"eng-42",
  "S":"BUY","o":"LIMIT","f":"GTC","q":"1.500","p":"153.45",
  "X":"NEW","x":"NEW","r":"NONE","i":4293153,"l":"0.000","z":"0.000",
  "L":"0.00","n":"0","N":null,"T":1499405658657,"t":-1,"w":true,"m":false,
  "C":"","Z":"0.00"
})";

/// A partial fill of that order: 0.400 at 153.44, as maker.
constexpr auto REPORT_PARTIAL_FILL = R"({
  "e":"executionReport","E":1499405658700,"s":"SOLUSDT","c":"eng-42",
  "S":"BUY","o":"LIMIT","f":"GTC","q":"1.500","p":"153.45",
  "X":"PARTIALLY_FILLED","x":"TRADE","r":"NONE","i":4293153,
  "l":"0.400","z":"0.400","L":"153.44","n":"0.001","N":"SOL",
  "T":1499405658690,"t":8827,"w":true,"m":true,"C":"","Z":"61.37"
})";

/// A cancel of the same order after that partial fill. Note the status.
constexpr auto REPORT_CANCEL_AFTER_PARTIAL = R"({
  "e":"executionReport","E":1499405658900,"s":"SOLUSDT","c":"eng-cancel-7",
  "S":"BUY","o":"LIMIT","f":"GTC","q":"1.500","p":"153.45",
  "X":"CANCELED","x":"CANCELED","r":"NONE","i":4293153,
  "l":"0.000","z":"0.400","L":"0.00","n":"0","N":null,
  "T":1499405658890,"t":-1,"w":false,"m":false,"C":"eng-42","Z":"61.37"
})";

/// A rejection, which the venue reports as a status rather than an HTTP error.
constexpr auto REPORT_REJECTED = R"({
  "e":"executionReport","E":1499405658658,"s":"SOLUSDT","c":"eng-43",
  "S":"BUY","o":"LIMIT","f":"GTC","q":"1.500","p":"153.45",
  "X":"REJECTED","x":"REJECTED","r":"INSUFFICIENT_BALANCE","i":0,
  "l":"0.000","z":"0.000","L":"0.00","T":1499405658657,"t":-1,
  "w":false,"m":false,"C":"","Z":"0.00"
})";

/// A different event on the same multiplexed stream.
constexpr auto REPORT_ACCOUNT_POSITION = R"({
  "e":"outboundAccountPosition","E":1564034571105,"u":1564034571073,
  "B":[{"a":"SOL","f":"10.0","l":"0.0"}]
})";

[[nodiscard]] auto decode(std::string_view json) {
	return parse_execution_report(json,
								  REPORT_PRICE_DECIMALS,
								  REPORT_QTY_DECIMALS);
}

} // namespace

TEST(BinanceParseExecutionReport, AnAcknowledgementCarriesNoFill) {
	const auto report = decode(REPORT_ACK);
	ASSERT_TRUE(report.has_value())
		<< message(report.error()) << ": a well-formed ack was refused";

	EXPECT_EQ(report->status, execution_status::accepted);
	EXPECT_EQ(report->kind, execution_kind::acknowledgement);
	EXPECT_FALSE(report->has_fill());
	EXPECT_EQ(report->last_qty_scaled, 0);
	EXPECT_EQ(report->cumulative_qty_scaled, 0);
	// Not terminal: everything interesting about this order is still to come.
	EXPECT_FALSE(report->is_terminal());
}

TEST(BinanceParseExecutionReport, TheOrdersOwnTermsAreReadAtTheListingScales) {
	const auto report = decode(REPORT_ACK);
	ASSERT_TRUE(report.has_value());

	EXPECT_EQ(report->symbol, "SOLUSDT");
	EXPECT_EQ(report->client_order_id, "eng-42");
	EXPECT_EQ(report->venue_order_id, 4'293'153);
	// 1.500 at three decimals and 153.45 at two - different scales on one
	// message, which is the ordinary case and the easy thing to get wrong.
	EXPECT_EQ(report->order_qty_scaled, 1500);
	EXPECT_EQ(report->order_price_scaled, 15345);
}

TEST(BinanceParseExecutionReport, ATradeCarriesTheQuantityThatJustExecuted) {
	const auto report = decode(REPORT_PARTIAL_FILL);
	ASSERT_TRUE(report.has_value()) << message(report.error());

	EXPECT_EQ(report->status, execution_status::partially_filled);
	EXPECT_EQ(report->kind, execution_kind::trade);
	EXPECT_TRUE(report->has_fill());
	EXPECT_EQ(report->last_qty_scaled, 400);
	EXPECT_EQ(report->last_price_scaled, 15344);
	EXPECT_EQ(report->cumulative_qty_scaled, 400);
	EXPECT_TRUE(report->is_maker);
	EXPECT_FALSE(report->is_terminal());
}

TEST(BinanceParseExecutionReport, ACancelOfAPartlyFilledOrderCarriesNoNewFill) {
	const auto report = decode(REPORT_CANCEL_AFTER_PARTIAL);
	ASSERT_TRUE(report.has_value()) << message(report.error());

	// The trap this test exists for: the venue reports the *cumulative* fill on
	// the cancel too, so a caller that books `z` on every message books this
	// order's 0.400 twice. `l` is what just happened, and it is zero.
	EXPECT_EQ(report->cumulative_qty_scaled, 400);
	EXPECT_EQ(report->last_qty_scaled, 0);
	EXPECT_FALSE(report->has_fill())
		<< "a cancellation must never look like a fill";

	EXPECT_EQ(report->kind, execution_kind::cancellation);
	EXPECT_EQ(report->status, execution_status::cancelled);
	EXPECT_TRUE(report->is_terminal());
}

TEST(BinanceParseExecutionReport, ACancelNamesTheOrderItCancelledAndNotItself) {
	const auto report = decode(REPORT_CANCEL_AFTER_PARTIAL);
	ASSERT_TRUE(report.has_value());

	// `c` is the cancel request's own id; `C` is the order that went away.
	// Retiring `c` would leave the real order working in our book for ever.
	EXPECT_EQ(report->client_order_id, "eng-cancel-7");
	EXPECT_EQ(report->original_client_order_id, "eng-42");
	EXPECT_EQ(report->subject_order_id(), "eng-42");
}

TEST(BinanceParseExecutionReport, AnOrdinaryReportIsItsOwnSubject) {
	const auto report = decode(REPORT_PARTIAL_FILL);
	ASSERT_TRUE(report.has_value());

	// `C` is an empty string rather than absent on every non-cancel report,
	// which must not be mistaken for "this report is about nothing".
	EXPECT_TRUE(report->original_client_order_id.empty());
	EXPECT_EQ(report->subject_order_id(), "eng-42");
}

TEST(BinanceParseExecutionReport, ARejectionKeepsTheVenuesOwnReason) {
	const auto report = decode(REPORT_REJECTED);
	ASSERT_TRUE(report.has_value()) << message(report.error());

	EXPECT_EQ(report->status, execution_status::rejected);
	EXPECT_EQ(report->kind, execution_kind::rejection);
	EXPECT_EQ(report->reject_reason, "INSUFFICIENT_BALANCE");
	EXPECT_TRUE(report->is_terminal());
}

TEST(BinanceParseExecutionReport, TheAbsenceOfAReasonIsNotAReason) {
	const auto report = decode(REPORT_ACK);
	ASSERT_TRUE(report.has_value());

	// The venue sends "NONE" where nothing was rejected. Carried through
	// verbatim it would read downstream as a rejection reason called NONE.
	EXPECT_TRUE(report->reject_reason.empty());
}

TEST(BinanceParseExecutionReport, AnotherEventTypeIsSkippedRatherThanRefused) {
	const auto report = decode(REPORT_ACCOUNT_POSITION);
	ASSERT_FALSE(report.has_value());

	// The stream is multiplexed. A balance update is a valid frame that is
	// simply not this one, and a caller must be able to tell that from a frame
	// it could not parse at all.
	EXPECT_EQ(report.error(), user_data_error::not_an_execution_report);
}

TEST(BinanceParseExecutionReport, AnUnknownStatusStillYieldsAUsableReport) {
	// A status the venue adds tomorrow must not cost us the fill on the same
	// message. The frame decodes; the status says it is not understood.
	constexpr auto invented = R"({
	  "e":"executionReport","E":1,"s":"SOLUSDT","c":"eng-9","X":"SOMETHING_NEW",
	  "x":"TRADE","i":1,"l":"0.100","z":"0.100","L":"153.44","q":"1.000",
	  "p":"153.45","T":1,"m":false,"C":"","r":"NONE"
	})";

	const auto report = decode(invented);
	ASSERT_TRUE(report.has_value()) << "a new status dropped the whole frame";

	EXPECT_EQ(report->status, execution_status::unknown);
	EXPECT_TRUE(report->has_fill());
	EXPECT_EQ(report->last_qty_scaled, 100);
}

TEST(BinanceParseExecutionReport, MalformedJsonIsRefused) {
	EXPECT_EQ(decode("{not json").error(), user_data_error::invalid_json);
	EXPECT_EQ(decode("").error(), user_data_error::invalid_json);
}

TEST(BinanceParseExecutionReport, AFrameWithNoEventTypeIsRefused) {
	EXPECT_EQ(decode(R"({"s":"SOLUSDT"})").error(),
			  user_data_error::missing_field);
}

TEST(BinanceParseExecutionReport, APresentButMalformedNumberIsRefused) {
	// Absent fields are tolerated - the venue omits most of them on most report
	// kinds - but a field that is *there* and is not a decimal is a wire
	// change, and guessing at it would put a wrong quantity into the book.
	constexpr auto bad = R"({
	  "e":"executionReport","E":1,"s":"SOLUSDT","c":"eng-9","X":"NEW","x":"NEW",
	  "i":1,"l":"not-a-number","z":"0.000","L":"0.00","q":"1.000","p":"153.45",
	  "T":1,"m":false,"C":"","r":"NONE"
	})";

	EXPECT_EQ(decode(bad).error(), user_data_error::bad_number);
}

TEST(BinanceParseExecutionReport, TransactionTimeFallsBackToEventTime) {
	constexpr auto no_transaction_time = R"({
	  "e":"executionReport","E":1499405658658,"s":"SOLUSDT","c":"eng-9",
	  "X":"NEW","x":"NEW","i":1,"C":"","r":"NONE"
	})";

	const auto report = decode(no_transaction_time);
	ASSERT_TRUE(report.has_value());
	// Better a slightly late timestamp than a zero one, which would read as
	// 1970 in every latency measurement downstream.
	EXPECT_EQ(report->transaction_time_ms, 1'499'405'658'658);
}
