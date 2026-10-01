#pragma once

// Internal building blocks of the network layer. Every connection owns a
// private io_context that is only run by the thread using the connection;
// blocking calls start asynchronous operations and run the io_context until
// they complete. Context cancellation and timeouts close the socket from the
// io_context thread, which makes pending operations fail immediately.

#include "puls/core/context.hpp"
#include "puls/core/error.hpp"
#include "puls/net/tls.hpp"
#include "puls/net/url.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/system/error_code.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace puls::net::detail {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using TlsStream = asio::ssl::stream<tcp::socket>;
using AnyStream = std::variant<tcp::socket, TlsStream>;
using Duration = std::chrono::nanoseconds;

tcp::socket& lowest_layer(AnyStream& stream);
void close_stream(AnyStream& stream) noexcept;

// Runs asynchronous operations to completion with cancellation and timeouts.
class OpRunner {
public:
    enum class Outcome { completed, canceled, timed_out };
    using Done = std::function<void()>;

    explicit OpRunner(asio::io_context& io) : io_(io) {}

    // start initiates the operation and must call done from its final
    // completion handler; abort closes the underlying socket(s) and runs on
    // the io_context thread when ctx finishes or the timeout expires.
    Outcome run(const Context& ctx, std::optional<Duration> timeout,
                const std::function<void(Done)>& start, const std::function<void()>& abort);

private:
    asio::io_context& io_;
};

struct ConnectOptions {
    std::shared_ptr<TlsContext> tls;
    Duration connect_timeout{};
    Duration tls_handshake_timeout{};
    // ALPN protocol announced during the TLS handshake.
    std::string alpn = "http/1.1";
};

// Resolves host names without blocking cancellation: resolution runs on a
// helper thread that is abandoned when ctx finishes first.
Result<std::vector<tcp::endpoint>> resolve(const Context& ctx, const std::string& host,
                                           const std::string& port);

// Opens a TCP or TLS stream to url (DNS, dual-stack connect, TLS with host
// name verification).
Result<AnyStream> establish(asio::io_context& io, OpRunner& runner, const Context& ctx,
                            const Url& url, const ConnectOptions& options);

// Default port of an http/https/ws/wss URL when it does not specify one.
std::string effective_port(const Url& url);
bool uses_tls(const Url& url);

// Error helpers that keep cancellation, timeout and transport details typed.
Error network_error(std::string message, bool timeout = false);
Error describe(const boost::system::error_code& code);
Error outcome_error(const Context& ctx, OpRunner::Outcome outcome,
                    const boost::system::error_code& code, std::string_view timeout_message);

} // namespace puls::net::detail
