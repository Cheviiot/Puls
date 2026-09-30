#include "puls/core/text.hpp"
#include "puls/net/http.hpp"
#include "puls/net/url.hpp"
#include "puls/net/websocket.hpp"

#include "support/mock_server.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace puls::net {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using testing::MockExchange;
using testing::MockServer;

HttpClientOptions test_options() {
    HttpClientOptions options;
    options.tls = testing::test_tls_context();
    return options;
}

Url parse_url(const std::string& text) {
    auto url = Url::parse(text);
    EXPECT_TRUE(url) << text;
    return url ? *url : Url{};
}

std::string read_body(const Context& ctx, HttpResponse& response) {
    auto body = response.read_up_to(ctx, 64 << 20);
    EXPECT_TRUE(body) << (body ? "" : body.error().message());
    return body ? *body : std::string();
}

TEST(Url, ParsesAbsoluteUrls) {
    const Url url = parse_url("https://CDN.example.com:8443/probes/50mb?lid=1&x=%20#frag");
    EXPECT_EQ(url.scheme, "https");
    EXPECT_EQ(url.host, "CDN.example.com:8443");
    EXPECT_EQ(url.hostname(), "CDN.example.com");
    EXPECT_EQ(url.port(), "8443");
    EXPECT_EQ(url.path, "/probes/50mb");
    EXPECT_EQ(url.raw_query, "lid=1&x=%20");
    EXPECT_EQ(url.fragment, "frag");
    EXPECT_EQ(url.request_uri(), "/probes/50mb?lid=1&x=%20");
    EXPECT_FALSE(url.has_user);

    const Url ipv6 = parse_url("wss://[::1]:443/ws");
    EXPECT_EQ(ipv6.host, "[::1]:443");
    EXPECT_EQ(ipv6.hostname(), "::1");
    EXPECT_TRUE(parse_url("https://user:pass@example.com/").has_user);
    EXPECT_EQ(parse_url("https://example.com").request_uri(), "/");
    EXPECT_EQ(parse_url("mailto:x@example.com").opaque, "x@example.com");
    EXPECT_EQ(parse_url("https://example.com/a%2Fb").request_uri(), "/a%2Fb");
    EXPECT_EQ(parse_url("https://example.com/a b").request_uri(), "/a%20b");
}

TEST(Url, RejectsMalformedUrls) {
    for (const char* invalid : {"http://[::1", "https://example.com:port/", "https://exa mple.com/",
                                "https://example.com/%zz", "://missing-scheme",
                                "https://example.com/\x01", "1http:bad/segment:colon"}) {
        EXPECT_FALSE(Url::parse(invalid)) << invalid;
    }
}

TEST(Url, ResolvesReferencesLikeRfc3986) {
    const Url base = parse_url("https://speedtest.ru/app/page/index.html?q=1");
    const auto resolve = [&base](const char* reference) {
        return base.resolve_reference(parse_url(reference)).to_string();
    };
    EXPECT_EQ(resolve("/static/app.js"), "https://speedtest.ru/static/app.js");
    EXPECT_EQ(resolve("chunk.js"), "https://speedtest.ru/app/page/chunk.js");
    EXPECT_EQ(resolve("../up.js"), "https://speedtest.ru/app/up.js");
    EXPECT_EQ(resolve("./a/./b/../c.js"), "https://speedtest.ru/app/page/a/c.js");
    EXPECT_EQ(resolve("//cdn.example/x.js"), "https://cdn.example/x.js");
    EXPECT_EQ(resolve("https://other.example/y.js?v=2"), "https://other.example/y.js?v=2");
    EXPECT_EQ(resolve(""), "https://speedtest.ru/app/page/index.html?q=1");
    EXPECT_EQ(resolve("?v=3"), "https://speedtest.ru/app/page/index.html?v=3");
}

TEST(Url, EncodesQueriesInSortedOrder) {
    auto values = parse_query("z=1&type=download&a=%3D&bad=%zz&semi;colon=1");
    set_query_value(values, "type", "upload");
    set_query_value(values, "duration", "5");
    EXPECT_EQ(encode_query(values), "a=%3D&duration=5&type=upload&z=1");
    EXPECT_EQ(query_escape("a b/ü"), "a+b%2F%C3%BC");
    EXPECT_EQ(*query_unescape("a+b%2F"), "a b/");
    EXPECT_FALSE(query_unescape("%4"));
}

TEST(Url, SplitsAndJoinsHostPort) {
    EXPECT_EQ(split_host_port("example.com:443")->first, "example.com");
    EXPECT_EQ(split_host_port("[::1]:80")->first, "::1");
    EXPECT_EQ(split_host_port("[::1]:80")->second, "80");
    EXPECT_EQ(split_host_port("a:b:c").error().message(),
              "address a:b:c: too many colons in address");
    EXPECT_EQ(split_host_port("example.com").error().message(),
              "address example.com: missing port in address");
    EXPECT_FALSE(split_host_port("[::1]"));
    EXPECT_FALSE(split_host_port("[::1]x:80"));
    EXPECT_EQ(join_host_port("::1", "20000"), "[::1]:20000");
    EXPECT_EQ(join_host_port("host", "1"), "host:1");
}

TEST(Http, ReusesKeepAliveConnection) {
    std::atomic<int> requests{0};
    MockServer server([&requests](MockExchange& exchange) {
        requests.fetch_add(1);
        exchange.respond(200, {{"Content-Type", "text/plain"}}, exchange.request().path);
    });
    HttpSession session(test_options());
    const Context ctx;
    for (int index = 0; index < 3; ++index) {
        HttpRequest request;
        request.url = parse_url(server.url("/ping/" + std::to_string(index)));
        request.headers = {{"User-Agent", "Puls test"}};
        auto response = session.send(ctx, request);
        ASSERT_TRUE(response) << response.error().message();
        EXPECT_EQ(response->status_code(), 200);
        EXPECT_EQ(response->status(), "200 OK");
        EXPECT_EQ(response->header("content-type"), "text/plain");
        EXPECT_EQ(read_body(ctx, *response), "/ping/" + std::to_string(index));
    }
    EXPECT_EQ(requests.load(), 3);
    EXPECT_EQ(server.connections(), 1);
}

TEST(Http, SendsRequestBodyAndHeaders) {
    std::string received;
    std::string content_length;
    std::string user_agent;
    MockServer server([&](MockExchange& exchange) {
        received = exchange.request().body;
        content_length = exchange.request().header("Content-Length");
        user_agent = exchange.request().header("User-Agent");
        exchange.respond(204);
    });
    HttpSession session(test_options());
    const std::string payload(2 << 20, 'x');
    HttpRequest request;
    request.method = "POST";
    request.url = parse_url(server.url("/upload.php?r=1"));
    request.headers = {{"User-Agent", "Puls test"}, {"Content-Type", "application/octet-stream"}};
    request.body = payload;
    auto response = session.send(Context(), request);
    ASSERT_TRUE(response) << response.error().message();
    EXPECT_EQ(response->status_code(), 204);
    EXPECT_TRUE(response->finished());
    EXPECT_EQ(received.size(), payload.size());
    EXPECT_EQ(content_length, std::to_string(payload.size()));
    EXPECT_EQ(user_agent, "Puls test");

    HttpRequest empty;
    empty.method = "POST";
    empty.url = parse_url(server.url("/api/server/gentoken"));
    ASSERT_TRUE(session.send(Context(), empty));
    EXPECT_EQ(content_length, "0");
}

TEST(Http, StreamsChunkedAndCloseDelimitedBodies) {
    const std::string body(300'000, 'b');
    MockServer server([&body](MockExchange& exchange) {
        if (exchange.request().path == "/chunked") {
            exchange.respond_chunked(200, {}, body);
            return;
        }
        exchange.write_raw("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
        exchange.write_raw(body);
        exchange.close();
    });
    HttpSession session(test_options());
    for (const char* path : {"/chunked", "/close"}) {
        HttpRequest request;
        request.url = parse_url(server.url(path));
        auto response = session.send(Context(), request);
        ASSERT_TRUE(response);
        EXPECT_FALSE(response->content_length().has_value());
        EXPECT_EQ(read_body(Context(), *response).size(), body.size());
    }
}

TEST(Http, TruncatedBodyReturnsBytesBeforeError) {
    MockServer server([](MockExchange& exchange) {
        exchange.respond(200, {{"Content-Length", "100"}}, std::string(10, 'x'));
        exchange.close();
    });
    HttpSession session(test_options());
    HttpRequest request;
    request.url = parse_url(server.url("/"));
    auto response = session.send(Context(), request);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->content_length(), 100u);
    std::array<char, 64> buffer{};
    std::size_t total = 0;
    Error failure;
    for (;;) {
        auto count = response->read(Context(), buffer);
        if (!count) {
            failure = count.error();
            break;
        }
        ASSERT_NE(*count, 0u);
        total += *count;
    }
    EXPECT_EQ(total, 10u);
    EXPECT_EQ(failure.message(), "unexpected EOF");
}

TEST(Http, ResponseHeaderTimeoutIsTypedTimeout) {
    MockServer server([](MockExchange& exchange) {
        std::this_thread::sleep_for(400ms);
        exchange.respond(200);
    });
    HttpClientOptions options = test_options();
    options.response_header_timeout = 50ms;
    HttpSession session(options);
    HttpRequest request;
    request.url = parse_url(server.url("/slow"));
    const auto started = Clock::now();
    auto response = session.send(Context(), request);
    ASSERT_FALSE(response);
    EXPECT_LT(Clock::now() - started, 300ms);
    ASSERT_NE(response.error().as<NetworkError>(), nullptr);
    EXPECT_TRUE(response.error().as<NetworkError>()->timeout);
    EXPECT_TRUE(text::contains(response.error().message(), "Get \"https://127.0.0.1:"));
}

TEST(Http, CancellationClosesBodyReadPromptly) {
    MockServer server([](MockExchange& exchange) {
        exchange.respond(200, {{"Content-Length", "1000000"}}, "partial");
        std::this_thread::sleep_for(2s);
        exchange.close();
    });
    HttpSession session(test_options());
    CancelScope scope{Context()};
    HttpRequest request;
    request.url = parse_url(server.url("/"));
    auto response = session.send(scope.context(), request);
    ASSERT_TRUE(response);
    std::thread canceler([&scope] {
        std::this_thread::sleep_for(50ms);
        scope.cancel();
    });
    const auto started = Clock::now();
    auto body = response->read_up_to(scope.context(), 1000000);
    canceler.join();
    ASSERT_FALSE(body);
    EXPECT_TRUE(body.error().is(errors::canceled_tag));
    EXPECT_LT(Clock::now() - started, 1s);
}

TEST(Http, CanceledContextFailsBeforeConnecting) {
    CancelScope scope{Context()};
    scope.cancel();
    HttpSession session(test_options());
    HttpRequest request;
    request.url = parse_url("https://127.0.0.1:1/");
    auto response = session.send(scope.context(), request);
    ASSERT_FALSE(response);
    EXPECT_TRUE(response.error().is(errors::canceled_tag));
}

TEST(Http, FollowsRedirectsOnlyWhenEnabledAndDropsKeysAcrossHosts) {
    std::string forwarded_key = "unset";
    MockServer target([&forwarded_key](MockExchange& exchange) {
        forwarded_key = exchange.request().header("x-api-key");
        exchange.respond(200, {}, "target");
    });
    MockServer origin([&target](MockExchange& exchange) {
        if (exchange.request().path == "/same") {
            exchange.respond(302, {{"Location", "/final"}});
            return;
        }
        if (exchange.request().path == "/final") {
            exchange.respond(200, {}, "final:" + exchange.request().header("x-api-key"));
            return;
        }
        exchange.respond(302, {{"Location", target.url("/elsewhere")}});
    });

    HttpSession plain(test_options());
    HttpRequest request;
    request.url = parse_url(origin.url("/same"));
    request.headers = {{"x-api-key", "secret"}};
    auto response = plain.send(Context(), request);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status_code(), 302);
    response->close();

    HttpClientOptions options = test_options();
    options.follow_redirects = true;
    HttpSession following(options);
    response = following.send(Context(), request);
    ASSERT_TRUE(response);
    EXPECT_EQ(read_body(Context(), *response), "final:secret");
    EXPECT_EQ(response->url().path, "/final");
    response->close();

    request.url = parse_url(origin.url("/cross"));
    response = following.send(Context(), request);
    ASSERT_TRUE(response);
    EXPECT_EQ(read_body(Context(), *response), "target");
    EXPECT_EQ(forwarded_key, "");
    response->close();

    request.method = "POST";
    request.url = parse_url(origin.url("/same"));
    response = following.send(Context(), request);
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status_code(), 302);
}

TEST(Http, RejectsUntrustedCertificate) {
    MockServer server([](MockExchange& exchange) { exchange.respond(200); });
    TlsOptions tls_options;
    tls_options.use_system_roots = false;
    auto untrusted = TlsContext::create(tls_options);
    ASSERT_TRUE(untrusted);
    HttpClientOptions options;
    options.tls = *untrusted;
    HttpSession session(options);
    HttpRequest request;
    request.url = parse_url(server.url("/"));
    auto response = session.send(Context(), request);
    ASSERT_FALSE(response);
    EXPECT_TRUE(text::contains(response.error().message(), "failed to verify certificate"))
        << response.error().message();
}

TEST(Http, RejectsCertificateForAnotherHost) {
    MockServer server([](MockExchange& exchange) { exchange.respond(200); },
                      MockServer::Transport::tls_wrong_name);
    HttpSession session(test_options());
    HttpRequest request;
    request.url = parse_url(server.url("/"));
    auto response = session.send(Context(), request);
    ASSERT_FALSE(response);
    EXPECT_TRUE(text::contains(response.error().message(), "mismatch"))
        << response.error().message();

    WebSocketOptions options;
    options.tls = testing::test_tls_context();
    EXPECT_FALSE(WebSocket::connect(Context(), parse_url(server.ws_url("/")), options));
}

TEST(Http, SupportsPlainHttpAndHostNames) {
    MockServer server([](MockExchange& exchange) { exchange.respond(200, {}, "plain"); }, false);
    HttpSession session;
    HttpRequest request;
    request.url = parse_url("http://localhost:" + std::to_string(server.port()) + "/");
    auto response = session.send(Context(), request);
    ASSERT_TRUE(response) << response.error().message();
    EXPECT_EQ(read_body(Context(), *response), "plain");
}

TEST(Http, ReportsConnectionRefused) {
    MockServer server([](MockExchange& exchange) { exchange.respond(200); });
    const auto port = server.port();
    HttpSession session(test_options());
    HttpRequest request;
    {
        MockServer placeholder([](MockExchange&) {});
        request.url = parse_url("https://127.0.0.1:" + std::to_string(placeholder.port()) + "/");
    }
    auto response = session.send(Context(), request);
    ASSERT_FALSE(response);
    EXPECT_TRUE(text::contains(response.error().message(), "dial tcp 127.0.0.1:"))
        << response.error().message();
    (void)port;
}

TEST(WebSocket, ExchangesTextAndBinaryMessages) {
    std::string upgrade_origin;
    MockServer server([&upgrade_origin](MockExchange& exchange) {
        upgrade_origin = exchange.request().header("Origin");
        auto& socket = exchange.websocket();
        while (auto message = socket.read()) {
            if (message->text) {
                socket.write_text("echo:" + message->payload);
            } else {
                socket.write_binary(message->payload);
            }
        }
    });
    WebSocketOptions options;
    options.tls = testing::test_tls_context();
    options.headers = {{"User-Agent", "Puls test"}, {"Origin", "https://yandex.ru"}};
    auto socket = WebSocket::connect(Context(), parse_url(server.ws_url("/ws?x=1")), options);
    ASSERT_TRUE(socket) << socket.error().message();
    ASSERT_FALSE((*socket)->write(Context(), true, "HI"));
    auto reply = (*socket)->read(Context());
    ASSERT_TRUE(reply);
    EXPECT_TRUE(reply->text);
    EXPECT_EQ(reply->payload, "echo:HI");
    ASSERT_FALSE((*socket)->write(Context(), false, std::string(1000, 'z')));
    reply = (*socket)->read(Context());
    ASSERT_TRUE(reply);
    EXPECT_FALSE(reply->text);
    EXPECT_EQ(reply->payload.size(), 1000u);
    EXPECT_EQ(upgrade_origin, "https://yandex.ru");
}

TEST(WebSocket, EnforcesReadLimitAndReportsClose) {
    MockServer server([](MockExchange& exchange) {
        auto& socket = exchange.websocket();
        if (exchange.request().path == "/big") {
            socket.write_text(std::string(5000, 'x'));
        } else {
            socket.close(1000);
        }
        (void)socket.read();
    });
    WebSocketOptions options;
    options.tls = testing::test_tls_context();
    options.read_limit = 4096;
    auto big = WebSocket::connect(Context(), parse_url(server.ws_url("/big")), options);
    ASSERT_TRUE(big);
    auto message = (*big)->read(Context());
    ASSERT_FALSE(message);
    EXPECT_EQ(message.error().message(), "websocket: read limit exceeded");

    auto closing = WebSocket::connect(Context(), parse_url(server.ws_url("/close")), options);
    ASSERT_TRUE(closing);
    message = (*closing)->read(Context());
    ASSERT_FALSE(message);
    EXPECT_TRUE(text::contains(message.error().message(), "websocket: close 1000"));
}

TEST(WebSocket, RejectsNonUpgradeResponse) {
    MockServer server([](MockExchange& exchange) { exchange.respond(404); });
    WebSocketOptions options;
    options.tls = testing::test_tls_context();
    auto socket = WebSocket::connect(Context(), parse_url(server.ws_url("/")), options);
    ASSERT_FALSE(socket);
    EXPECT_EQ(socket.error().message(), "websocket: bad handshake");
}

TEST(WebSocket, HandshakeTimeoutStopsStalledServer) {
    MockServer server([](MockExchange&) { std::this_thread::sleep_for(500ms); });
    WebSocketOptions options;
    options.tls = testing::test_tls_context();
    options.handshake_timeout = 50ms;
    const auto started = Clock::now();
    auto socket = WebSocket::connect(Context(), parse_url(server.ws_url("/")), options);
    ASSERT_FALSE(socket);
    EXPECT_LT(Clock::now() - started, 400ms);
}

TEST(WebSocket, DuplexWritesWhileReadingAcknowledgements) {
    MockServer server([](MockExchange& exchange) {
        auto& socket = exchange.websocket();
        while (auto message = socket.read()) {
            if (message->text || !socket.write_text(std::to_string(message->payload.size()))) {
                return;
            }
        }
    });
    WebSocketOptions options;
    options.tls = testing::test_tls_context();
    auto socket = WebSocket::connect(Context(), parse_url(server.ws_url("/")), options);
    ASSERT_TRUE(socket);
    const std::string chunk(64 << 10, 'u');
    std::int64_t sent = 0;
    std::int64_t acknowledged = 0;
    CancelScope scope(Context(), 150ms);
    DuplexHandlers handlers;
    handlers.next_payload = [&] {
        sent += static_cast<std::int64_t>(chunk.size());
        return std::span<const char>(chunk.data(), chunk.size());
    };
    handlers.write_failed = [&] { sent -= static_cast<std::int64_t>(chunk.size()); };
    handlers.on_message = [&](const WebSocketMessage& message) -> Error {
        acknowledged += *text::atoi(message.payload).value;
        return {};
    };
    const DuplexResult result = (*socket)->duplex(scope.context(), handlers, DuplexOptions{});
    EXPECT_TRUE(result.canceled);
    EXPECT_GT(acknowledged, 0);
    EXPECT_LE(acknowledged, sent);
}

} // namespace
} // namespace puls::net
