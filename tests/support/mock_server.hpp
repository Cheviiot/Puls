#pragma once

#include "puls/net/tls.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Local HTTP(S) and WebSocket servers for network tests. Every connection is
// served synchronously by its own thread; handlers may block and sleep.
namespace puls::testing {

using Headers = std::vector<std::pair<std::string, std::string>>;

struct MockRequest {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    Headers headers;
    std::string body;
    bool websocket_upgrade = false;

    [[nodiscard]] std::string header(std::string_view name) const;
    [[nodiscard]] std::string query_value(std::string_view key) const;
};

struct MockMessage {
    bool text = false;
    std::string payload;
};

class MockWebSocket {
public:
    virtual ~MockWebSocket() = default;
    // Returns nothing when the connection is closed or fails.
    virtual std::optional<MockMessage> read() = 0;
    virtual bool write_text(std::string_view payload) = 0;
    virtual bool write_binary(std::string_view payload) = 0;
    // Sends a close frame with the given status code.
    virtual void close(std::uint16_t code = 1000) = 0;
};

class MockExchange {
public:
    virtual ~MockExchange() = default;

    [[nodiscard]] virtual const MockRequest& request() const = 0;
    // Writes a complete response; Content-Length is added unless present.
    virtual void respond(int status, const Headers& headers = {}, std::string_view body = {}) = 0;
    // Writes a response with chunked transfer encoding.
    virtual void respond_chunked(int status, const Headers& headers, std::string_view body) = 0;
    // Writes raw bytes to the connection.
    virtual void write_raw(std::string_view bytes) = 0;
    // Closes the connection once the handler returns.
    virtual void close() = 0;
    // Completes the WebSocket upgrade for this request.
    virtual MockWebSocket& websocket() = 0;
};

class MockServer {
public:
    using Handler = std::function<void(MockExchange&)>;
    // tls_wrong_name serves a trusted certificate issued for another host.
    enum class Transport { tls, plain, tls_wrong_name };

    explicit MockServer(Handler handler, Transport transport = Transport::tls);
    MockServer(Handler handler, bool tls)
        : MockServer(std::move(handler), tls ? Transport::tls : Transport::plain) {}
    MockServer(const MockServer&) = delete;
    MockServer& operator=(const MockServer&) = delete;
    ~MockServer();

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    // "127.0.0.1:port"
    [[nodiscard]] std::string host() const;
    // "https://127.0.0.1:port" + path (http:// without TLS).
    [[nodiscard]] std::string url(std::string_view path = {}) const;
    // "wss://127.0.0.1:port" + path (ws:// without TLS).
    [[nodiscard]] std::string ws_url(std::string_view path = {}) const;
    [[nodiscard]] int connections() const noexcept { return accepted_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint16_t port_ = 0;
    bool tls_ = true;
    std::atomic<int> accepted_{0};
};

// A client TLS context that trusts only the test certificate.
std::shared_ptr<net::TlsContext> test_tls_context();
// PEM encoded self-signed certificate and key for 127.0.0.1, ::1, localhost.
const std::string& test_certificate_pem();
const std::string& test_private_key_pem();

} // namespace puls::testing
