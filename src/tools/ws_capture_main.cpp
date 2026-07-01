// Captures a Binance diff-depth WebSocket stream to a JSONL file for offline
// replay (see benchmark/market_replay.cpp). One `depthUpdate` frame per line —
// exactly what binance::parse_binance_depth_updates ingests.
//
//   ws_capture_tool SYMBOL OUTFILE [seconds=30] [speed=100ms|1000ms]
//
// e.g.  ws_capture_tool SOLUSDT sol.jsonl 60 100ms
//
// The socket lives only in this recording path; the benchmark never opens one.
// @see https://developers.binance.com/docs/binance-spot-api-docs/web-socket-streams

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <expected>
#include <fstream>
#include <string>
#include <string_view>

#include <fmt/std.h>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

namespace {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace websocket = beast::websocket;
    namespace ssl = asio::ssl;
    using tcp = asio::ip::tcp;

    // as_tuple turns each completion into a tuple led by the error_code, so failures
    // stay values (no exceptions across co_await) — the same token snapshot_tool uses.
    inline constexpr auto token = asio::as_tuple(asio::use_awaitable);

    /**
     * @brief Stream a Binance diff-depth WebSocket to @p outfile, one frame per line.
     * @param host Endpoint host (e.g. @c stream.binance.com).
     * @param port Endpoint port (e.g. @c 9443).
     * @param target Stream path (e.g. @c /ws/solusdt@depth@100ms).
     * @param outfile Destination JSONL file (truncated).
     * @param duration How long to record before closing.
     * @return Nothing on success, or a human-readable error string.
     */
    asio::awaitable<std::expected<void, std::string> >
    capture(std::string host, std::string port, std::string target,
            std::string outfile, std::chrono::seconds duration) {
        using namespace std::chrono_literals;
        const auto executor = co_await asio::this_coro::executor;

        ssl::context ctx(ssl::context::tls_client);
        ctx.set_default_verify_paths();
        // Public market data only; skip cert verification so we don't depend on a CA
        // bundle. Do NOT copy this for anything sensitive.
        ctx.set_verify_mode(ssl::verify_none);

        tcp::resolver resolver(executor);
        websocket::stream<beast::ssl_stream<beast::tcp_stream> > ws(
            executor, ctx);

        // SNI — Binance requires it for the TLS handshake.
        if (SSL_set_tlsext_host_name(ws.next_layer().native_handle(),
                                     host.c_str()) == 0) {
            co_return std::unexpected("failed to set TLS SNI host name");
        }

        auto [resolve_ec, endpoints] =
                co_await resolver.async_resolve(host, port, token);
        if (resolve_ec)
            co_return std::unexpected("resolve: " + resolve_ec.message());

        beast::get_lowest_layer(ws).expires_after(10s);
        auto [connect_ec, endpoint] =
                co_await beast::get_lowest_layer(ws).async_connect(endpoints,
                    token);
        if (connect_ec)
            co_return std::unexpected("connect: " + connect_ec.message());

        if (auto [handshake_ec] =
                    co_await ws.next_layer().async_handshake(
                        ssl::stream_base::client, token);
            handshake_ec)
            co_return std::unexpected(
                "tls handshake: " + handshake_ec.message());

        // Hand timeout management to the websocket layer (ping keepalive + idle
        // timeout); the raw tcp deadline must be cleared or it fights the ws stream.
        beast::get_lowest_layer(ws).expires_never();
        ws.set_option(websocket::stream_base::timeout::suggested(
            beast::role_type::client));
        ws.set_option(
            websocket::stream_base::decorator([](websocket::request_type &req) {
                req.set(http::field::user_agent, "order_book/1.0");
            }));

        // RFC 6455 Host header carries the port for the ws upgrade.
        const std::string host_header = host + ':' + port;
        if (auto [ws_ec] = co_await ws.async_handshake(host_header, target,
                token);
            ws_ec)
            co_return std::unexpected("ws handshake: " + ws_ec.message());

        std::ofstream out(outfile, std::ios::binary | std::ios::trunc);
        if (!out)
            co_return std::unexpected("cannot open output file: " + outfile);

        const auto deadline = std::chrono::steady_clock::now() + duration;
        std::uint64_t frames = 0;
        beast::flat_buffer buffer;
        while (std::chrono::steady_clock::now() < deadline) {
            auto [read_ec, bytes] = co_await ws.async_read(buffer, token);
            if (read_ec) {
                if (read_ec == websocket::error::closed) break; // server closed
                co_return std::unexpected("read: " + read_ec.message());
            }
            const auto line = beast::buffers_to_string(buffer.data());
            out.write(line.data(), static_cast<std::streamsize>(line.size()));
            out.put('\n');
            buffer.consume(buffer.size());
            if (++frames % 50 == 0) fmt::print(stderr, "\rframes: {}", frames);
        }
        out.flush();

        // Best-effort graceful close; a truncated close from the server is fine.
        auto _ =
                co_await ws.async_close(websocket::close_code::normal, token);

        fmt::println(stderr, "\ncaptured {} frames to {}", frames, outfile);
        co_return std::expected<void, std::string>{};
    }

    void print_usage() {
        fmt::println(stderr,
                     "usage:\n"
                     "  ws_capture_tool SYMBOL OUTFILE [seconds=30] [speed=100ms|1000ms]\n"
                     "e.g.\n"
                     "  ws_capture_tool SOLUSDT sol.jsonl 60 100ms");
    }
} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        print_usage();
        return EXIT_FAILURE;
    }

    // Binance stream names are lowercase.
    std::string symbol = argv[1];
    std::ranges::transform(symbol, symbol.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });

    const std::string outfile = argv[2];
    const int seconds = argc > 3 ? std::atoi(argv[3]) : 30;
    const std::string_view speed = argc > 4 ? argv[4] : "100ms";
    if (seconds <= 0) {
        fmt::println(stderr, "seconds must be positive");
        return EXIT_FAILURE;
    }

    // @depth pushes every 1000ms; @depth@100ms every 100ms (Binance spot).
    const std::string stream = speed == "1000ms"
                                   ? symbol + "@depth"
                                   : symbol + "@depth@100ms";
    const std::string target = "/ws/" + stream;

    asio::io_context ioc;
    std::expected<void, std::string> result = std::unexpected("not run");
    asio::co_spawn(
        ioc,
        capture("stream.binance.com", "9443", target, outfile,
                std::chrono::seconds(seconds)),
        [&result](const std::exception_ptr &ep,
                  std::expected<void, std::string> r) {
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

    if (!result) {
        fmt::println(stderr, "capture error: {}", result.error());
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
