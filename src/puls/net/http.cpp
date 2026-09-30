#include "puls/net/http.hpp"

#include "puls/core/text.hpp"
#include "puls/net/detail/transport.hpp"

#include <boost/asio/ssl/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/buffer_body.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/span_body.hpp>
#include <boost/beast/http/write.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace puls::net {

namespace {

namespace http = boost::beast::http;
using detail::OpRunner;

constexpr std::size_t max_header_bytes = 1 << 20;
constexpr int max_redirects = 10;

bool idempotent(std::string_view method) {
    return method == "GET" || method == "HEAD" || method == "OPTIONS" || method == "TRACE";
}

bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

// Go's url.Error operation name: "Get", "Post".
std::string operation_name(std::string_view method) {
    if (method.empty()) {
        return "Get";
    }
    std::string name(1, method.front());
    name += text::to_lower_ascii(method.substr(1));
    return name;
}

bool sensitive_header(std::string_view name) {
    return text::equal_fold_ascii(name, "authorization") ||
           text::equal_fold_ascii(name, "cookie") || text::equal_fold_ascii(name, "cookie2") ||
           text::equal_fold_ascii(name, "www-authenticate") ||
           text::equal_fold_ascii(name, "x-api-key") || text::equal_fold_ascii(name, "jwt");
}

struct ResponseHead {
    int status_code = 0;
    std::string status;
    std::vector<Header> headers;
    std::optional<std::uint64_t> content_length;
    bool finished = false;
};

bool retryable_read_failure(const boost::system::error_code& code) {
    return code == boost::asio::error::eof || code == http::error::end_of_stream ||
           code == boost::asio::error::connection_reset ||
           code == boost::asio::ssl::error::stream_truncated;
}

} // namespace

struct HttpSession::Impl {
    explicit Impl(HttpClientOptions client_options)
        : options(std::move(client_options)), runner(io) {}

    HttpClientOptions options;
    detail::asio::io_context io;
    OpRunner runner;
    std::optional<detail::AnyStream> stream;
    std::string origin;
    bool reusable = false;
    bool response_open = false;
    bool keep_alive = false;
    boost::beast::flat_buffer buffer;
    std::optional<http::response_parser<http::buffer_body>> parser;
    // The response currently reading from this session, detached when the
    // session is closed or destroyed first.
    HttpResponse* open_response = nullptr;

    void detach_response() noexcept;

    void close_connection() noexcept {
        if (stream) {
            detail::close_stream(*stream);
            stream.reset();
        }
        reusable = false;
        keep_alive = false;
        origin.clear();
        buffer.clear();
    }

    // An idle keep-alive connection may have been closed by the server.
    bool connection_alive() {
        if (!stream || buffer.size() > 0) {
            return false;
        }
        auto& socket = detail::lowest_layer(*stream);
        boost::system::error_code error;
        socket.non_blocking(true, error);
        if (error) {
            return false;
        }
        char byte = 0;
        socket.receive(boost::asio::buffer(&byte, 1), detail::tcp::socket::message_peek, error);
        boost::system::error_code ignored;
        socket.non_blocking(false, ignored);
        return error == boost::asio::error::would_block;
    }

    Result<ResponseHead> round_trip(const Context& ctx, const std::string& method, const Url& url,
                                    const std::vector<Header>& headers, std::string_view body);
};

Result<ResponseHead> HttpSession::Impl::round_trip(const Context& ctx, const std::string& method,
                                                   const Url& url,
                                                   const std::vector<Header>& headers,
                                                   std::string_view body) {
    const std::string target_origin =
        url.scheme + "://" + join_host_port(url.hostname(), detail::effective_port(url));
    bool reuse = stream && reusable && origin == target_origin && connection_alive();

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!reuse) {
            close_connection();
            detail::ConnectOptions connect;
            connect.tls = options.tls;
            connect.connect_timeout = options.connect_timeout;
            connect.tls_handshake_timeout = options.tls_handshake_timeout;
            auto established = detail::establish(io, runner, ctx, url, connect);
            if (!established) {
                return std::move(established).error();
            }
            stream.emplace(std::move(established).value());
            origin = target_origin;
        }
        reusable = false;

        http::request<http::span_body<const char>> request;
        const http::verb verb = http::string_to_verb(method);
        if (verb == http::verb::unknown) {
            request.method_string(method);
        } else {
            request.method(verb);
        }
        request.target(url.request_uri());
        request.version(11);
        request.set(http::field::host, url.host);
        for (const auto& header : headers) {
            request.set(header.name, header.value);
        }
        request.body() = {body.data(), body.size()};
        if (!body.empty() || method == "POST" || method == "PUT" || method == "PATCH") {
            request.content_length(body.size());
        }

        boost::system::error_code write_error;
        std::size_t written = 0;
        auto outcome = runner.run(
            ctx, std::nullopt,
            [&](OpRunner::Done done) {
                std::visit(
                    [&](auto& value) {
                        http::async_write(
                            value, request,
                            [&write_error, &written, done](const boost::system::error_code& error,
                                                           std::size_t size) {
                                write_error = error;
                                written = size;
                                done();
                            });
                    },
                    *stream);
            },
            [this] { detail::close_stream(*stream); });
        if (outcome != OpRunner::Outcome::completed || write_error) {
            Error error = detail::outcome_error(ctx, outcome, write_error, "i/o timeout");
            close_connection();
            if (reuse && attempt == 0 && !ctx.done() && (idempotent(method) || written == 0)) {
                reuse = false;
                continue;
            }
            return error;
        }

        parser.emplace();
        parser->body_limit(std::numeric_limits<std::uint64_t>::max());
        parser->header_limit(static_cast<std::uint32_t>(max_header_bytes));
        if (method == "HEAD") {
            parser->skip(true);
        }
        boost::system::error_code read_error;
        std::size_t header_bytes = 0;
        outcome = runner.run(
            ctx,
            options.response_header_timeout > Duration::zero()
                ? std::optional(options.response_header_timeout)
                : std::nullopt,
            [&](OpRunner::Done done) {
                std::visit(
                    [&](auto& value) {
                        http::async_read_header(
                            value, buffer, *parser,
                            [&read_error, &header_bytes,
                             done](const boost::system::error_code& error, std::size_t size) {
                                read_error = error;
                                header_bytes = size;
                                done();
                            });
                    },
                    *stream);
            },
            [this] { detail::close_stream(*stream); });
        if (outcome != OpRunner::Outcome::completed || read_error) {
            Error error = detail::outcome_error(ctx, outcome, read_error,
                                                "net/http: timeout awaiting response headers");
            const bool nothing_received = header_bytes == 0 && buffer.size() == 0;
            close_connection();
            parser.reset();
            if (reuse && attempt == 0 && !ctx.done() && outcome == OpRunner::Outcome::completed &&
                idempotent(method) && nothing_received && retryable_read_failure(read_error)) {
                reuse = false;
                continue;
            }
            return error;
        }

        ResponseHead head;
        const auto& message = parser->get();
        head.status_code = static_cast<int>(message.result_int());
        head.status = std::to_string(head.status_code);
        if (const std::string_view reason(message.reason().data(), message.reason().size());
            !reason.empty()) {
            head.status += " ";
            head.status += reason;
        }
        for (const auto& field : message) {
            head.headers.push_back(
                Header{std::string(field.name_string().data(), field.name_string().size()),
                       std::string(field.value().data(), field.value().size())});
        }
        if (const auto length = parser->content_length()) {
            head.content_length = *length;
        }
        keep_alive = message.keep_alive();
        head.finished = parser->is_done();
        response_open = true;
        return head;
    }
    return detail::network_error("EOF");
}

void HttpSession::Impl::detach_response() noexcept {
    if (open_response != nullptr) {
        open_response->session_ = nullptr;
        open_response = nullptr;
    }
    response_open = false;
}

HttpSession::HttpSession(HttpClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

HttpSession::~HttpSession() {
    close();
}

void HttpSession::close() noexcept {
    impl_->detach_response();
    impl_->close_connection();
    impl_->parser.reset();
}

Result<HttpResponse> HttpSession::send(const Context& ctx, const HttpRequest& request) {
    Url url = request.url;
    std::string method = request.method.empty() ? std::string("GET") : request.method;
    std::vector<Header> headers = request.headers;
    const auto fail = [&method, &url](Error cause) {
        return Error::wrap(operation_name(method) + " " + text::quote(url.to_string()),
                           std::move(cause));
    };
    if (impl_->response_open) {
        impl_->detach_response();
        impl_->close_connection();
        impl_->parser.reset();
    }
    if (url.scheme != "http" && url.scheme != "https") {
        return fail(Error::make("unsupported protocol scheme " + text::quote(url.scheme)));
    }
    if (url.host.empty()) {
        return fail(Error::make("http: no Host in request URL"));
    }
    const std::string target = url.request_uri();
    if (std::any_of(target.begin(), target.end(), [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return byte <= 0x20 || byte == 0x7F;
        })) {
        return fail(Error::make("net/http: can't write control character in Request.URL"));
    }
    if (ctx.done()) {
        return fail(ctx.err());
    }

    for (int redirects = 0;; ++redirects) {
        auto head = impl_->round_trip(ctx, method, url, headers, request.body);
        if (!head) {
            return fail(std::move(head).error());
        }
        HttpResponse response;
        response.session_ = this;
        impl_->open_response = &response;
        response.status_code_ = head->status_code;
        response.status_ = std::move(head->status);
        response.headers_ = std::move(head->headers);
        response.content_length_ = head->content_length;
        response.finished_ = head->finished;
        response.url_ = url;
        if (!impl_->options.follow_redirects || !is_redirect(response.status_code()) ||
            (method != "GET" && method != "HEAD")) {
            return response;
        }
        const std::string location = response.header("Location");
        if (location.empty()) {
            return response;
        }
        if (redirects >= max_redirects) {
            response.close();
            return fail(Error::make("stopped after 10 redirects"));
        }
        auto reference = Url::parse(location);
        if (!reference) {
            response.close();
            return fail(Error::wrap("failed to parse Location header " + text::quote(location),
                                    reference.error()));
        }
        Url next = url.resolve_reference(*reference);
        if (next.scheme != "http" && next.scheme != "https") {
            response.close();
            return fail(Error::make("unsupported protocol scheme " + text::quote(next.scheme)));
        }
        if (url.scheme == "https" && next.scheme != "https") {
            response.close();
            return fail(Error::make("redirect to insecure URL is not allowed"));
        }
        (void)response.discard(ctx, 64 << 10);
        response.close();
        if (!text::equal_fold_ascii(next.host, url.host)) {
            headers.erase(
                std::remove_if(headers.begin(), headers.end(),
                               [](const Header& header) { return sensitive_header(header.name); }),
                headers.end());
        }
        url = std::move(next);
    }
}

HttpResponse::HttpResponse(HttpResponse&& other) noexcept
    : session_(std::exchange(other.session_, nullptr)), status_code_(other.status_code_),
      status_(std::move(other.status_)), headers_(std::move(other.headers_)),
      content_length_(other.content_length_), url_(std::move(other.url_)),
      finished_(other.finished_), pending_error_(std::move(other.pending_error_)) {
    if (session_ != nullptr) {
        session_->impl_->open_response = this;
    }
}

HttpResponse& HttpResponse::operator=(HttpResponse&& other) noexcept {
    if (this != &other) {
        close();
        session_ = std::exchange(other.session_, nullptr);
        status_code_ = other.status_code_;
        status_ = std::move(other.status_);
        headers_ = std::move(other.headers_);
        content_length_ = other.content_length_;
        url_ = std::move(other.url_);
        finished_ = other.finished_;
        pending_error_ = std::move(other.pending_error_);
        if (session_ != nullptr) {
            session_->impl_->open_response = this;
        }
    }
    return *this;
}

HttpResponse::~HttpResponse() {
    close();
}

std::string HttpResponse::header(std::string_view name) const {
    for (const auto& header : headers_) {
        if (text::equal_fold_ascii(header.name, name)) {
            return header.value;
        }
    }
    return {};
}

Result<std::size_t> HttpResponse::read(const Context& ctx, std::span<char> buffer) {
    if (pending_error_) {
        return pending_error_;
    }
    if (finished_) {
        return std::size_t{0};
    }
    if (session_ == nullptr || !session_->impl_->parser || !session_->impl_->stream) {
        return Error::make("http: read on closed response body");
    }
    if (buffer.empty()) {
        return std::size_t{0};
    }
    auto& impl = *session_->impl_;
    for (;;) {
        auto& body = impl.parser->get().body();
        body.data = buffer.data();
        body.size = buffer.size();
        boost::system::error_code code;
        const auto outcome = impl.runner.run(
            ctx, std::nullopt,
            [&](OpRunner::Done done) {
                std::visit(
                    [&](auto& value) {
                        http::async_read_some(
                            value, impl.buffer, *impl.parser,
                            [&code, done](const boost::system::error_code& error, std::size_t) {
                                code = error;
                                done();
                            });
                    },
                    *impl.stream);
            },
            [&impl] { detail::close_stream(*impl.stream); });
        if (code == http::error::need_buffer) {
            code = {};
        }
        // Servers often end close-delimited HTTPS bodies without a TLS
        // close_notify; like Go, accept that as the end of such a body. Bodies
        // with a declared length still fail when they are cut short.
        if (code == boost::asio::ssl::error::stream_truncated && impl.parser->need_eof()) {
            code = {};
            impl.parser->put_eof(code);
        }
        const std::size_t delivered = buffer.size() - impl.parser->get().body().size;
        if (outcome != OpRunner::Outcome::completed || code) {
            pending_error_ = detail::outcome_error(ctx, outcome, code, "i/o timeout");
            impl.close_connection();
            if (delivered > 0) {
                return delivered;
            }
            return pending_error_;
        }
        if (impl.parser->is_done()) {
            finished_ = true;
        }
        if (delivered > 0 || finished_) {
            return delivered;
        }
    }
}

Result<std::string> HttpResponse::read_up_to(const Context& ctx, std::size_t max_bytes) {
    std::string data;
    std::array<char, 16 << 10> chunk{};
    while (data.size() < max_bytes) {
        const std::size_t want = std::min(chunk.size(), max_bytes - data.size());
        auto count = read(ctx, std::span<char>(chunk.data(), want));
        if (!count) {
            return std::move(count).error();
        }
        if (*count == 0) {
            break;
        }
        data.append(chunk.data(), *count);
    }
    return data;
}

Result<std::uint64_t> HttpResponse::discard(const Context& ctx, std::uint64_t max_bytes) {
    std::uint64_t total = 0;
    std::array<char, 16 << 10> chunk{};
    while (total < max_bytes) {
        const auto want =
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk.size(), max_bytes - total));
        auto count = read(ctx, std::span<char>(chunk.data(), want));
        if (!count) {
            return std::move(count).error();
        }
        if (*count == 0) {
            break;
        }
        total += *count;
    }
    return total;
}

void HttpResponse::close() noexcept {
    if (session_ == nullptr) {
        return;
    }
    auto& impl = *session_->impl_;
    if (finished_ && impl.keep_alive && !pending_error_ && impl.stream) {
        impl.reusable = true;
    } else {
        impl.close_connection();
    }
    impl.parser.reset();
    impl.response_open = false;
    impl.open_response = nullptr;
    session_ = nullptr;
}

} // namespace puls::net
