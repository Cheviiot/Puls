#pragma once

#include "puls/core/context.hpp"
#include "puls/core/error.hpp"
#include "puls/net/tls.hpp"
#include "puls/net/url.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace puls::net {

using Duration = std::chrono::nanoseconds;

struct Header {
    std::string name;
    std::string value;
};

struct HttpRequest {
    std::string method = "GET";
    Url url;
    std::vector<Header> headers;
    // Not owned; must stay valid until send() returns.
    std::string_view body;
};

// Transport settings of an HTTP/1.1 client. Connections are direct: proxies
// are never used because they would distort measurements.
struct HttpClientOptions {
    // Null selects TlsContext::system().
    std::shared_ptr<TlsContext> tls;
    // Zero disables the limit; the request context still applies.
    Duration connect_timeout{};
    Duration tls_handshake_timeout{std::chrono::seconds(10)};
    // Time allowed between sending the request and receiving response headers.
    Duration response_header_timeout{std::chrono::seconds(10)};
    // Follows at most 10 redirects of GET/HEAD requests. Sensitive headers are
    // dropped when the host changes and HTTPS is never downgraded.
    bool follow_redirects = false;
};

class HttpSession;

// A response whose body is streamed from the connection. At most one
// response per session may be open; closing it (or destroying it) returns a
// completely read keep-alive connection to the session and closes any other.
class HttpResponse {
public:
    HttpResponse(HttpResponse&& other) noexcept;
    HttpResponse& operator=(HttpResponse&& other) noexcept;
    HttpResponse(const HttpResponse&) = delete;
    HttpResponse& operator=(const HttpResponse&) = delete;
    ~HttpResponse();

    [[nodiscard]] int status_code() const noexcept { return status_code_; }
    // Status line text such as "200 OK".
    [[nodiscard]] const std::string& status() const noexcept { return status_; }
    // First value of a header (case-insensitive), or empty.
    [[nodiscard]] std::string header(std::string_view name) const;
    // Declared body length; empty for chunked or close-delimited bodies.
    [[nodiscard]] std::optional<std::uint64_t> content_length() const noexcept {
        return content_length_;
    }
    // Final URL after redirects.
    [[nodiscard]] const Url& url() const noexcept { return url_; }

    // Reads the next part of the body into buffer. Returns 0 at the end of the
    // body. Bytes received before a transport error are returned first; the
    // error is reported by the following call.
    Result<std::size_t> read(const Context& ctx, std::span<char> buffer);
    // Reads at most max_bytes of the body.
    Result<std::string> read_up_to(const Context& ctx, std::size_t max_bytes);
    // Reads and discards at most max_bytes; returns the number of bytes read.
    Result<std::uint64_t> discard(const Context& ctx, std::uint64_t max_bytes);
    // Completely read bodies keep the connection reusable.
    [[nodiscard]] bool finished() const noexcept { return finished_; }
    void close() noexcept;

private:
    friend class HttpSession;
    HttpResponse() = default;

    HttpSession* session_ = nullptr;
    int status_code_ = 0;
    std::string status_;
    std::vector<Header> headers_;
    std::optional<std::uint64_t> content_length_;
    Url url_;
    bool finished_ = false;
    Error pending_error_;
};

// HttpSession sends requests over at most one keep-alive connection. It is
// not thread-safe; every worker thread uses its own session. Cancellation of
// the request context closes the connection immediately.
class HttpSession {
public:
    explicit HttpSession(HttpClientOptions options = {});
    HttpSession(const HttpSession&) = delete;
    HttpSession& operator=(const HttpSession&) = delete;
    ~HttpSession();

    // Sends request and returns once response headers are received. Errors
    // are formatted like Go's url.Error: `Get "https://host/path": cause`.
    Result<HttpResponse> send(const Context& ctx, const HttpRequest& request);
    // Closes the current connection.
    void close() noexcept;

private:
    friend class HttpResponse;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace puls::net
