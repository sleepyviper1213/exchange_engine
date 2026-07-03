// Fetches a Binance depth snapshot over HTTPS (Boost.Beast + C++20 coroutines)
// or reads a saved one, loads it into an OrderBook, and prints top of book.
//
//   snapshot_tool SOLUSDT [limit=100] [priceDecimals=2] [qtyDecimals=2]   # live
//   snapshot_tool --file sol.json [priceDecimals=2] [qtyDecimals=2]       # offline

#include <chrono>
#include <cstdlib>
#include <exception>
#include <expected>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>

#include <fmt/std.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

#include "io/binance_depth.hpp"
#include "engine/order_book.hpp"

namespace {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace ssl = asio::ssl;
    using tcp = asio::ip::tcp;
    // as_tuple delivers each completion as a tuple led by the error_code, so
    // failures stay values (no exceptions thrown across co_await) and each step
    // unpacks its own result with a structured binding.
    inline constexpr auto token = asio::as_tuple(asio::use_awaitable);

    /**
     *  HTTPS GET
     * @param host
     * @param target
     * @return
     */
    asio::awaitable<std::expected<std::string, std::string> >
    https_get(std::string host, std::string target) {
        const auto executor = co_await asio::this_coro::executor;

        ssl::context ctx(ssl::context::tls_client);
        ctx.set_default_verify_paths();
        // Dev tool fetching public market data: skip cert verification so we don't
        // depend on a CA bundle being installed. Do NOT do this for anything
        // sensitive.
        ctx.set_verify_mode(ssl::verify_none);

        tcp::resolver resolver(executor);
        beast::ssl_stream<beast::tcp_stream> stream(executor, ctx);

        // SNI — many hosts (incl. Binance) require it for the TLS handshake.
        if (SSL_set_tlsext_host_name(stream.native_handle(),
                                     host.c_str()) == 0) {
            co_return std::unexpected("failed to set TLS SNI host name");
        }


        auto [resolve_ec, endpoints] =
                co_await resolver.async_resolve(host, "443", token);
        if (resolve_ec)
            co_return std::unexpected(
                "resolve: " + resolve_ec.message());

        using namespace std::chrono_literals;
        beast::get_lowest_layer(stream).expires_after(10s);
        auto [connect_ec, connected_ep] =
                co_await beast::get_lowest_layer(stream).async_connect(
                    endpoints, token);
        if (connect_ec)
            co_return std::unexpected(
                "connect: " + connect_ec.message());

        if (auto [handshake_ec] =
                    co_await stream.
                    async_handshake(ssl::stream_base::client, token);
            handshake_ec)
            co_return std::unexpected(
                "tls handshake: " + handshake_ec.message());

        http::request<http::empty_body> req{http::verb::get, target, 11};
        req.set(http::field::host, host);
        req.set(http::field::user_agent, "order_book/1.0");
        req.set(http::field::accept, "application/json");

        beast::get_lowest_layer(stream).expires_after(10s);
        auto [write_ec, bytes_written] =
                co_await http::async_write(stream, req, token);
        if (write_ec) co_return std::unexpected("write: " + write_ec.message());

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        auto [read_ec, bytes_read] =
                co_await http::async_read(stream, buffer, res, token);
        if (read_ec) co_return std::unexpected("read: " + read_ec.message());

        std::string body = std::move(res.body());
        const unsigned status = res.result_int();

        // Best-effort TLS shutdown; servers often close without close_notify
        // (stream_truncated), which is fine here.
        auto [_] = co_await stream.async_shutdown(token);

        if (status != 200) {
            co_return std::unexpected(fmt::format("HTTP {}: {}", status, body));
        }
        co_return body;
    }

    std::expected<std::string, std::string> fetch_depth(std::string_view symbol,
        int limit) {
        const std::string target =
                fmt::format("/api/v3/depth?symbol={}&limit={}", symbol, limit);

        asio::io_context ioc;
        std::expected<std::string, std::string> result = std::unexpected(
            "not run");
        asio::co_spawn(
            ioc, https_get("api.binance.com", target),
            [&result](std::exception_ptr ep,
                      std::expected<std::string, std::string> r) {
                if (ep) {
                    try {
                        std::rethrow_exception(ep);
                    } catch (const std::exception &e) {
                        result = std::unexpected(
                            std::string("exception: ") + e.what());
                    }
                } else {
                    result = std::move(r);
                }
            });
        ioc.run();
        return result;
    }

    std::expected<std::string, std::string> read_file(const char *path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::unexpected(fmt::format("cannot open {}", path));
        std::ostringstream buffer;
        buffer << in.rdbuf();
        return buffer.str();
    }

    void print_usage() {
        fmt::println(
            stderr,
            "usage:\n"
            "  snapshot_tool SYMBOL [limit=100] [priceDecimals=2] [qtyDecimals=2]   # live fetch\n"
            "  snapshot_tool --file <depth.json> [priceDecimals=2] [qtyDecimals=2]  # offline");
    }
} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return EXIT_FAILURE;
    }

    std::expected<std::string, std::string> json = std::unexpected("uninit");
    int price_decimals = 2;
    int qty_decimals = 2;
    using clock = std::chrono::steady_clock;
    const auto t_fetch_begin = clock::now();

    if (std::string_view(argv[1]) == "--file") {
        if (argc < 3) {
            print_usage();
            return EXIT_FAILURE;
        }
        price_decimals = argc > 3 ? std::atoi(argv[3]) : 2;
        qty_decimals = argc > 4 ? std::atoi(argv[4]) : 2;
        json = read_file(argv[2]);
    } else {
        const std::string symbol = argv[1];
        const int limit = argc > 2 ? std::atoi(argv[2]) : 100;
        price_decimals = argc > 3 ? std::atoi(argv[3]) : 2;
        qty_decimals = argc > 4 ? std::atoi(argv[4]) : 2;
        json = fetch_depth(symbol, limit);
    }

    const auto t_fetch_end = clock::now();

    if (!json) {
        fmt::println(stderr, "fetch error: {}", json.error());
        return EXIT_FAILURE;
    }

    const auto t_parse_begin = clock::now();
    const auto snapshot =
            binance::parse_binance_depth(*json, price_decimals, qty_decimals);
    if (!snapshot) {
        fmt::println(stderr, "parse error: {}", snapshot.error());
        return EXIT_FAILURE;
    }
    const auto t_parse_end = clock::now();

    const auto t_build_begin = clock::now();
    OrderBook book;
    for (const auto &[price, volume]: snapshot->bids) {
        book.add_order(Side::BID, price, volume);
    }
    for (const auto &[price, volume]: snapshot->asks) {
        book.add_order(Side::ASK, price, volume);
    }
    const auto t_build_end = clock::now();

    // Report the three phases separately: the fetch is a network round trip
    // (DNS + TCP + TLS handshake + HTTP) and dwarfs the CPU work, so a single
    // combined "elapsed" hides that parse+build are microsecond-scale.
    fmt::println(
        "fetch={}  parse={}  build={}\n"
        "lastUpdateId={}  bids={}  asks={}",
        std::chrono::duration_cast<std::chrono::microseconds>(
            t_fetch_end - t_fetch_begin),
        std::chrono::duration_cast<std::chrono::microseconds>(
            t_parse_end - t_parse_begin),
        std::chrono::duration_cast<std::chrono::microseconds>(
            t_build_end - t_build_begin),
        snapshot->lastUpdateId,
        snapshot->bids.size(), snapshot->asks.size());

    const auto bid = book.best_bid();
    const auto ask = book.best_ask();
    if (bid && ask) {
        fmt::println("best bid={}  best ask={}  spread={} ticks", *bid,
                     *ask, *ask - *bid);
    }
    return EXIT_SUCCESS;
}
