#pragma once

#include "puls/core/context.hpp"
#include "puls/core/error.hpp"
#include "puls/net/http.hpp"
#include "puls/net/tls.hpp"
#include "puls/net/url.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace puls::net {

struct WebSocketOptions {
    // Null selects TlsContext::system().
    std::shared_ptr<TlsContext> tls;
    // Zero disables the limit; the context still applies.
    Duration connect_timeout{};
    // Limit for the TLS handshake and the HTTP upgrade together.
    Duration handshake_timeout{std::chrono::seconds(10)};
    // Larger messages fail with a read limit error.
    std::size_t read_limit = 64 << 10;
    std::vector<Header> headers;
};

struct WebSocketMessage {
    bool text = false;
    std::string payload;
};

// Callbacks of WebSocket::duplex. They run on the thread that called duplex.
struct DuplexHandlers {
    // Returns the next binary payload to send; called before every write.
    std::function<std::span<const char>()> next_payload;
    // Called when a write fails, after next_payload has been called for it.
    std::function<void()> write_failed;
    // Handles a received message; a non-empty error stops the exchange.
    std::function<Error(const WebSocketMessage&)> on_message;
};

struct DuplexOptions {
    // Maximum time one write may take.
    Duration write_timeout{std::chrono::seconds(5)};
    // Pause between consecutive writes.
    Duration write_gap{std::chrono::milliseconds(1)};
};

struct DuplexResult {
    // The context finished.
    bool canceled = false;
    // Reading failed (including a closed connection).
    Error read_error;
    // Writing failed.
    Error write_error;
    // on_message rejected a message.
    Error handler_error;
};

// A client WebSocket connection. Not thread-safe; used by one thread.
class WebSocket {
public:
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;
    ~WebSocket();

    // Connects to a ws:// or wss:// URL and completes the upgrade handshake.
    static Result<std::unique_ptr<WebSocket>> connect(const Context& ctx, const Url& url,
                                                      const WebSocketOptions& options);

    Result<WebSocketMessage> read(const Context& ctx);
    Error write(const Context& ctx, bool text, std::string_view payload);
    // Sends binary payloads continuously while reading messages, until the
    // context finishes, a read or write fails or on_message returns an error.
    DuplexResult duplex(const Context& ctx, const DuplexHandlers& handlers,
                        const DuplexOptions& options);
    void close() noexcept;

private:
    struct Impl;
    explicit WebSocket(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace puls::net
