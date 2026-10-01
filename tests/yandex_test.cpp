#include "puls/core/json.hpp"
#include "puls/core/text.hpp"
#include "puls/service/yandex/backend.hpp"

#include "support/mock_server.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace puls::yandex {

struct Backend::TestAccess {
    static void set_latency_urls(Backend& backend, std::vector<std::string> urls) {
        std::lock_guard lock(backend.mutex_);
        backend.latency_urls_ = std::move(urls);
    }
    static std::vector<std::string> latency_urls(Backend& backend) {
        std::lock_guard lock(backend.mutex_);
        return backend.latency_urls_;
    }
    static std::vector<std::string> download_urls(Backend& backend) {
        std::lock_guard lock(backend.mutex_);
        return backend.download_urls_;
    }
    static Result<std::int64_t> download_probe(Backend& backend, const std::string& url,
                                               std::size_t buffer_size,
                                               const measure::ReadyFn& ready,
                                               const measure::RecordFn& record) {
        net::HttpSession session(backend.http_options());
        std::vector<char> buffer(buffer_size);
        return backend.download_probe(Context(), session, url, buffer, ready, record);
    }
    static Error upload_worker(Backend& backend, const Context& ctx, const std::string& post_url,
                               const std::string& websocket_url, std::string_view payload,
                               const measure::ReadyFn& ready, const measure::RecordFn& record) {
        net::HttpSession session(backend.http_options());
        const UploadProbe probe{post_url, websocket_url, {}};
        service::MeasurementConfig config;
        config.duration = std::chrono::seconds(3);
        return backend.upload_worker(ctx, session, probe, payload, config, ready, record);
    }
    static Error websocket_upload(Backend& backend, const Context& ctx, const std::string& url,
                                  std::chrono::nanoseconds connection_timeout,
                                  const measure::ReadyFn& ready, const measure::RecordFn& record) {
        return backend.websocket_upload(ctx, url, connection_timeout, std::chrono::seconds(3),
                                        ready, record);
    }
    static Error http_upload(Backend& backend, const std::string& url, std::string_view payload,
                             const measure::ReadyFn& ready, const measure::RecordFn& record) {
        net::HttpSession session(backend.http_options());
        return backend.http_upload(Context(), session, url, payload, ready, record);
    }
};

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Access = Backend::TestAccess;
using testing::MockExchange;
using testing::MockServer;

const testing::Headers octet_stream = {{"Content-Type", "application/octet-stream"}};
const testing::Headers json_type = {{"Content-Type", "application/json"}};

Options test_options(std::string probes_url = {}) {
    Options options;
    options.tls = testing::test_tls_context();
    if (!probes_url.empty()) {
        options.probes_url = std::move(probes_url);
    }
    return options;
}

std::string websocket_base(const MockServer& server) {
    return server.ws_url();
}

boost::json::object discovery_document(const MockServer& server) {
    const std::string base = server.url();
    return {
        {"mid", "test"},
        {"latency", {{"probes", boost::json::array{{{"url", base + "/ping"}}}}}},
        {"download",
         {{"probes", boost::json::array{{{"url", base + "/probes/50mb"}, {"timeout", 0}},
                                        {{"url", base + "/probes/100kb"}, {"timeout", 100}}}}}},
        {"upload",
         {{"probes", boost::json::array{{{"size", 30720},
                                         {"url", base + "/upload"},
                                         {"postUrl", base + "/upload-http"},
                                         {"statsUrl", base + "/upload-stats"},
                                         {"websocketUrl", websocket_base(server) + "/upload-ws"},
                                         {"websocketConnectionTimeout", 2000},
                                         {"timeout", 0}}}}}},
    };
}

std::string serialize(const boost::json::value& value) {
    return *json::encode_indented(value);
}

boost::json::object& probe(boost::json::object& document, const char* section, std::size_t index) {
    return document[section].as_object()["probes"].as_array()[index].as_object();
}

TEST(YandexProtocol, HostOf) {
    EXPECT_EQ(detail::host_of("https://cdn.example.com/path?query=1"), "cdn.example.com");
    EXPECT_EQ(detail::host_of("http://cdn.example.com/path"), "cdn.example.com");
    EXPECT_EQ(detail::host_of("https://cdn.example.com"), "cdn.example.com");
    EXPECT_EQ(detail::endpoint_host("https://CDN.Example.com:443/x"), "cdn.example.com:443");
}

TEST(YandexProtocol, CacheBustAddsUniqueQueryParameter) {
    EXPECT_TRUE(text::contains(detail::cache_bust("https://example.com/probe"), "?cb="));
    EXPECT_TRUE(text::contains(detail::cache_bust("https://example.com/probe?lid=1"), "&cb="));
    EXPECT_NE(detail::cache_bust("https://example.com/probe"),
              detail::cache_bust("https://example.com/probe"));
}

TEST(YandexProtocol, ProbeUrlValidation) {
    EXPECT_TRUE(detail::valid_http_probe_url("https://cdn.example/probe"));
    EXPECT_FALSE(detail::valid_http_probe_url("http://cdn.example/probe"));
    EXPECT_FALSE(detail::valid_http_probe_url("https://user@cdn.example/probe"));
    EXPECT_FALSE(detail::valid_http_probe_url("https://cdn.example/probe#fragment"));
    EXPECT_TRUE(detail::valid_websocket_probe_url("wss://cdn.example/upload"));
    EXPECT_FALSE(detail::valid_websocket_probe_url("ws://cdn.example/upload"));
    EXPECT_TRUE(detail::is_large_download_probe("https://cdn.example/probes/50mb/"));
    EXPECT_FALSE(detail::is_large_download_probe("https://cdn.example/probes/50mb-copy"));
}

TEST(YandexProtocol, ExtractBalancedJsonObjectHandlesEscapesAndNesting) {
    const auto object = detail::extract_balanced_json_object(
        R"(prefix Client.default({"a":"va\"lue","b":{"c":1}}) suffix)", "Client.default(");
    ASSERT_TRUE(object);
    EXPECT_EQ(*object, R"({"a":"va\"lue","b":{"c":1}})");
    EXPECT_FALSE(detail::extract_balanced_json_object("no marker", "Client.default("));
    EXPECT_FALSE(detail::extract_balanced_json_object("Client.default( [1]", "Client.default("));
    EXPECT_FALSE(detail::extract_balanced_json_object("Client.default({\"a\":", "Client.default("));
}

TEST(Yandex, DiscoveryPingAndDownloadProtocol) {
    std::atomic<int> ping_requests{0};
    MockServer* self = nullptr;
    MockServer server([&](MockExchange& exchange) {
        const std::string& path = exchange.request().path;
        if (path == "/get-probes") {
            boost::json::object document = discovery_document(*self);
            probe(document, "latency", 0)["url"] = self->url("/latency");
            exchange.respond(200, json_type, serialize(document));
        } else if (path == "/latency") {
            ping_requests.fetch_add(1);
            exchange.respond(204);
        } else if (path == "/probes/50mb" || path == "/download") {
            exchange.respond(200, octet_stream, std::string(32 << 10, 'd'));
        } else {
            exchange.respond(404);
        }
    });
    self = &server;
    Backend backend(test_options(server.url("/get-probes")));
    const auto selected = backend.select_server(Context());
    ASSERT_TRUE(selected) << selected.error().message();
    EXPECT_EQ(selected->name, server.host());

    const auto ping = backend.ping(Context());
    ASSERT_TRUE(ping) << ping.error().message();
    EXPECT_EQ(ping->samples, 4);
    EXPECT_EQ(ping->method, "minimum");
    EXPECT_EQ(ping_requests.load(), 4);
    EXPECT_EQ(server.connections(), 2);

    bool ready = false;
    std::int64_t recorded = 0;
    const auto bytes = Access::download_probe(
        backend, server.url("/download"), 4096, [&ready] { ready = true; },
        [&recorded](std::int64_t count) { recorded += count; });
    ASSERT_TRUE(bytes) << bytes.error().message();
    EXPECT_EQ(*bytes, 32 << 10);
    EXPECT_EQ(recorded, 32 << 10);
    EXPECT_TRUE(ready);
}

TEST(Yandex, PingIsSequentialPerCdnAndParallelAcrossCdns) {
    std::mutex mutex;
    std::condition_variable arrived;
    int active_a = 0, active_b = 0, max_a = 0, max_b = 0, max_total = 0;
    bool seen_a = false, seen_b = false;
    MockServer server([&](MockExchange& exchange) {
        const bool is_a = exchange.request().path == "/a";
        if (!is_a && exchange.request().path != "/b") {
            exchange.respond(404);
            return;
        }
        int& active = is_a ? active_a : active_b;
        int& maximum = is_a ? max_a : max_b;
        {
            std::unique_lock lock(mutex);
            ++active;
            maximum = std::max(maximum, active);
            max_total = std::max(max_total, active_a + active_b);
            (is_a ? seen_a : seen_b) = true;
            arrived.notify_all();
            // The first request to each CDN waits for the other CDN, so the
            // overlap does not depend on how fast connections are set up. A
            // sequential implementation never satisfies the wait.
            arrived.wait_for(lock, 3s, [&] { return seen_a && seen_b; });
        }
        std::this_thread::sleep_for(15ms);
        {
            const std::lock_guard lock(mutex);
            --active;
        }
        exchange.respond(204);
    });
    Backend backend(test_options());
    Access::set_latency_urls(backend, {server.url("/a"), server.url("/b")});
    const auto result = backend.ping(Context());
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result->samples, 8);
    const std::lock_guard lock(mutex);
    EXPECT_EQ(max_a, 1);
    EXPECT_EQ(max_b, 1);
    EXPECT_EQ(max_total, 2);
}

TEST(Yandex, PingKeepsValidSamplesAfterOneRequestFails) {
    std::atomic<int> requests{0};
    MockServer server([&requests](MockExchange& exchange) {
        if (requests.fetch_add(1) == 0) {
            exchange.respond(503, {}, "temporary");
            return;
        }
        exchange.respond(204);
    });
    Backend backend(test_options());
    Access::set_latency_urls(backend, {server.url("/")});
    const auto result = backend.ping(Context());
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(requests.load(), 4);
    EXPECT_EQ(result->samples, 3);
}

TEST(Yandex, PingRequiresSelectedServerAndReportsFailures) {
    Backend backend(test_options());
    const auto missing = backend.ping(Context());
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().as<service::OpError>()->code, service::ErrorCode::internal);

    MockServer server([](MockExchange& exchange) { exchange.respond(503); });
    Access::set_latency_urls(backend, {server.url("/")});
    const auto failed = backend.ping(Context());
    ASSERT_FALSE(failed);
    const auto* operation = failed.error().as<service::OpError>();
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->code, service::ErrorCode::unavailable);
    EXPECT_TRUE(operation->retryable);
}

TEST(Yandex, DiscoveryRejectsMalformedSchemaWithoutReplacingState) {
    std::atomic<bool> malformed{false};
    MockServer* self = nullptr;
    MockServer server([&](MockExchange& exchange) {
        boost::json::object document = discovery_document(*self);
        if (malformed.load()) {
            probe(document, "upload", 0)["postUrl"] = "https://different-cdn.example/upload";
        }
        exchange.respond(200, json_type, serialize(document));
    });
    self = &server;
    Backend backend(test_options(server.url()));
    ASSERT_TRUE(backend.select_server(Context()));
    const auto latency = Access::latency_urls(backend);
    const auto download = Access::download_urls(backend);
    malformed.store(true);
    EXPECT_FALSE(backend.select_server(Context()));
    EXPECT_EQ(Access::latency_urls(backend), latency);
    EXPECT_EQ(Access::download_urls(backend), download);
}

TEST(Yandex, DiscoveryValidatesEveryProbe) {
    const std::vector<std::pair<std::string, std::function<void(boost::json::object&)>>> cases = {
        {"latency URL",
         [](boost::json::object& document) {
             probe(document, "latency", 0)["url"] = "http://insecure.example/ping";
         }},
        {"optional download URL",
         [](boost::json::object& document) {
             document["download"].as_object()["probes"].as_array().push_back(
                 {{"url", "http://insecure.example/probes/100kb"}, {"timeout", 100}});
         }},
        {"main download path",
         [](boost::json::object& document) {
             probe(document, "download", 0)["url"] = "https://cdn.example/probes/50mb-copy";
         }},
        {"websocket timeout",
         [](boost::json::object& document) {
             probe(document, "upload", 0)["websocketConnectionTimeout"] = 0;
         }},
        {"optional upload URL",
         [](boost::json::object& document) {
             boost::json::object invalid = probe(document, "upload", 0);
             invalid["url"] = "http://insecure.example/upload";
             invalid["timeout"] = 100;
             document["upload"].as_object()["probes"].as_array().push_back(invalid);
         }},
        {"empty mid", [](boost::json::object& document) { document["mid"] = " "; }},
        {"wrong field type",
         [](boost::json::object& document) { probe(document, "download", 0)["timeout"] = "0"; }},
        {"fractional number",
         [](boost::json::object& document) { probe(document, "upload", 0)["size"] = 1.5; }},
    };
    for (const auto& [name, mutate] : cases) {
        MockServer* self = nullptr;
        const auto& change = mutate;
        MockServer server([&](MockExchange& exchange) {
            boost::json::object document = discovery_document(*self);
            change(document);
            exchange.respond(200, json_type, serialize(document));
        });
        self = &server;
        Backend backend(test_options(server.url()));
        const auto selected = backend.select_server(Context());
        ASSERT_FALSE(selected) << name;
        const auto* operation = selected.error().as<service::OpError>();
        ASSERT_NE(operation, nullptr) << name;
        EXPECT_EQ(operation->code, service::ErrorCode::protocol) << name;
    }
}

TEST(Yandex, DiscoveryRejectsUnexpectedContentTypeAndOversizedBody) {
    MockServer server([](MockExchange& exchange) {
        if (exchange.request().path == "/type") {
            exchange.respond(200, {{"Content-Type", "text/html"}}, "{}");
            return;
        }
        exchange.respond(200, json_type,
                         R"({"mid":"test","padding":")" + std::string(2 << 20, 'x') + R"("})");
    });
    for (const char* path : {"/type", "/size"}) {
        Backend backend(test_options(server.url(path)));
        EXPECT_FALSE(backend.select_server(Context())) << path;
    }
}

TEST(Yandex, DiscoveryClassifiesHttpStatus) {
    struct Case {
        int status;
        service::ErrorCode code;
        bool retryable;
    };
    for (const Case& test : {Case{403, service::ErrorCode::protocol, false},
                             Case{429, service::ErrorCode::protocol, true},
                             Case{503, service::ErrorCode::unavailable, true}}) {
        MockServer server([&test](MockExchange& exchange) { exchange.respond(test.status); });
        Backend backend(test_options(server.url()));
        const auto selected = backend.select_server(Context());
        ASSERT_FALSE(selected);
        const auto* operation = selected.error().as<service::OpError>();
        ASSERT_NE(operation, nullptr);
        EXPECT_EQ(operation->code, test.code) << test.status;
        EXPECT_EQ(operation->retryable, test.retryable) << test.status;
    }
}

TEST(Yandex, DiscoveryDoesNotFollowRedirects) {
    std::atomic<bool> redirected{false};
    MockServer server([&redirected](MockExchange& exchange) {
        if (exchange.request().path == "/target") {
            redirected.store(true);
            exchange.respond(200, json_type, "{}");
            return;
        }
        exchange.respond(302, {{"Location", "/target"}});
    });
    Backend backend(test_options(server.url("/start")));
    EXPECT_FALSE(backend.select_server(Context()));
    EXPECT_FALSE(redirected.load());
}

TEST(Yandex, DownloadRejectsTruncatedBody) {
    MockServer server([](MockExchange& exchange) {
        exchange.respond(200, {{"Content-Length", "100"}, octet_stream[0]}, std::string(10, 'x'));
        exchange.close();
    });
    Backend backend(test_options());
    std::int64_t recorded = 0;
    const auto bytes = Access::download_probe(
        backend, server.url("/"), 32, [] {},
        [&recorded](std::int64_t count) { recorded += count; });
    EXPECT_FALSE(bytes);
    EXPECT_EQ(recorded, 10);
}

TEST(Yandex, DownloadRejectsHttp5xxWithoutBytes) {
    MockServer server([](MockExchange& exchange) { exchange.respond(503, {}, "failed"); });
    Backend backend(test_options());
    std::int64_t recorded = 0;
    const auto bytes = Access::download_probe(
        backend, server.url("/"), 32, [] {},
        [&recorded](std::int64_t count) { recorded += count; });
    ASSERT_FALSE(bytes);
    EXPECT_EQ(recorded, 0);
    EXPECT_EQ(service::classify_error(bytes.error()), service::ErrorCode::unavailable);
}

TEST(Yandex, DownloadValidatesHeadersBeforeBecomingReady) {
    MockServer server([](MockExchange& exchange) {
        const std::string& path = exchange.request().path;
        if (path == "/unknown-length") {
            exchange.respond_chunked(200, octet_stream, "data");
        } else if (path == "/encoding") {
            exchange.respond(200, {octet_stream[0], {"Content-Encoding", "gzip"}}, "data");
        } else if (path == "/type") {
            exchange.respond(200, {{"Content-Type", "text/plain"}}, "data");
        } else {
            exchange.respond(
                200, {octet_stream[0], {"Content-Length", std::to_string((1LL << 30) + 1)}}, "");
            exchange.close();
        }
    });
    Backend backend(test_options());
    for (const char* path : {"/unknown-length", "/encoding", "/type", "/unsafe-size"}) {
        bool ready = false;
        std::int64_t recorded = 0;
        const auto bytes = Access::download_probe(
            backend, server.url(path), 32, [&ready] { ready = true; },
            [&recorded](std::int64_t count) { recorded += count; });
        ASSERT_FALSE(bytes) << path;
        EXPECT_EQ(service::classify_error(bytes.error()), service::ErrorCode::protocol) << path;
        EXPECT_FALSE(ready) << path;
        EXPECT_EQ(recorded, 0) << path;
    }
}

TEST(Yandex, DownloadValidatesNative50MiBProbeSize) {
    MockServer server([](MockExchange& exchange) {
        exchange.respond(200, octet_stream, std::string(1024, 'x'));
    });
    Backend backend(test_options());
    bool ready = false;
    std::int64_t recorded = 0;
    const auto bytes = Access::download_probe(
        backend, server.url("/probes/50mb"), 1024, [&ready] { ready = true; },
        [&recorded](std::int64_t count) { recorded += count; });
    EXPECT_FALSE(bytes);
    EXPECT_FALSE(ready);
    EXPECT_EQ(recorded, 0);
}

TEST(Yandex, UploadWebSocketFallsBackToConfirmedHttp) {
    std::atomic<int> http_requests{0};
    MockServer server([&http_requests](MockExchange& exchange) {
        if (exchange.request().path == "/ws") {
            auto& socket = exchange.websocket();
            (void)socket.read();
            socket.write_text(R"({"k":"u","b":1000000000000})");
            return;
        }
        http_requests.fetch_add(1);
        exchange.respond(204);
    });
    std::atomic<bool> fallback_logged{false};
    Options options = test_options();
    options.log = [&fallback_logged](std::string_view message) {
        if (text::contains(message, "резервный")) {
            fallback_logged.store(true);
        }
    };
    Backend backend(options);
    const CancelScope scope(Context(), 150ms);
    std::int64_t confirmed = 0;
    const Error error = Access::upload_worker(
        backend, scope.context(), server.url("/upload"), server.ws_url("/ws"),
        std::string(1024, '\0'), [] {}, [&confirmed](std::int64_t count) { confirmed += count; });
    EXPECT_TRUE(error.is(errors::deadline_exceeded_tag)) << error.message();
    EXPECT_GT(confirmed, 0);
    EXPECT_GT(http_requests.load(), 0);
    EXPECT_TRUE(fallback_logged.load());
}

TEST(Yandex, WebSocketUploadAcceptsZeroControlAck) {
    MockServer server([](MockExchange& exchange) {
        auto& socket = exchange.websocket();
        bool first = true;
        while (auto message = socket.read()) {
            if (message->text) {
                return;
            }
            if (first) {
                socket.write_text(R"({"k":"u","b":0,"i":0})");
                first = false;
            }
            socket.write_text(R"({"k":"u","b":)" + std::to_string(message->payload.size()) + "}");
        }
    });
    Backend backend(test_options());
    const CancelScope scope(Context(), 100ms);
    std::int64_t confirmed = 0;
    const Error error = Access::websocket_upload(
        backend, scope.context(), server.ws_url(), {}, [] {},
        [&confirmed](std::int64_t count) { confirmed += count; });
    EXPECT_TRUE(error.is(errors::deadline_exceeded_tag)) << error.message();
    EXPECT_GT(confirmed, 0);
}

TEST(Yandex, WebSocketUploadValidatesAcknowledgementSchema) {
    struct Case {
        const char* name;
        bool text;
        std::string payload;
    };
    const std::vector<Case> cases = {
        {"missing bytes", true, R"({"k":"u"})"},
        {"wrong command", true, R"({"k":"download","b":1})"},
        {"negative bytes", true, R"({"k":"u","b":-1})"},
        {"wrong bytes type", true, R"({"k":"u","b":"1"})"},
        {"multiple JSON values", true, R"({"k":"u","b":1} {})"},
        {"binary acknowledgement", false, R"({"k":"u","b":1})"},
        {"oversized acknowledgement", true,
         R"({"k":"u","b":1,"padding":")" + std::string(4 << 10, 'x') + R"("})"},
    };
    for (const Case& test : cases) {
        MockServer server([&test](MockExchange& exchange) {
            auto& socket = exchange.websocket();
            if (!socket.read()) {
                return;
            }
            if (test.text) {
                socket.write_text(test.payload);
            } else {
                socket.write_binary(test.payload);
            }
            (void)socket.read();
        });
        Backend backend(test_options());
        const CancelScope scope(Context(), 1s);
        const Error error = Access::websocket_upload(
            backend, scope.context(), server.ws_url(), {}, [] {}, [](std::int64_t) {});
        ASSERT_TRUE(error) << test.name;
        EXPECT_FALSE(error.is(errors::deadline_exceeded_tag)) << test.name;
        EXPECT_FALSE(text::contains(error.message(), "padding")) << test.name;
    }
}

TEST(Yandex, WebSocketUploadCountsOnlyAcknowledgedBinaryFrame) {
    std::atomic<bool> query_ok{false};
    MockServer server([&query_ok](MockExchange& exchange) {
        if (exchange.request().query_value("type") == "upload" &&
            exchange.request().query_value("duration") == "5") {
            query_ok.store(true);
        }
        auto& socket = exchange.websocket();
        const auto message = socket.read();
        if (!message || message->text) {
            return;
        }
        socket.write_text(R"({"k":"u","b":)" + std::to_string(message->payload.size()) +
                          R"(,"i":1})");
        socket.close(1000);
    });
    Backend backend(test_options());
    bool ready = false;
    std::int64_t confirmed = 0;
    const Error error = Access::websocket_upload(
        backend, Context(), server.ws_url("/upload?existing=1"), {}, [&ready] { ready = true; },
        [&confirmed](std::int64_t count) { confirmed += count; });
    EXPECT_TRUE(error);
    EXPECT_TRUE(query_ok.load());
    EXPECT_TRUE(ready);
    EXPECT_EQ(confirmed, 64 << 10);
}

TEST(Yandex, WebSocketUploadHonorsDiscoveryConnectionTimeout) {
    MockServer server([](MockExchange&) { std::this_thread::sleep_for(1s); });
    Backend backend(test_options());
    const CancelScope scope(Context(), 5s);
    const auto started = Clock::now();
    const Error error = Access::websocket_upload(
        backend, scope.context(), server.ws_url(), 30ms, [] {}, [](std::int64_t) {});
    EXPECT_TRUE(error);
    EXPECT_LT(Clock::now() - started, 700ms);
}

TEST(Yandex, HttpUploadRejectsOversizedResponseWithoutConfirmation) {
    MockServer server([](MockExchange& exchange) {
        exchange.respond(200, {{"Content-Length", std::to_string((1 << 20) + 1)}}, "");
        exchange.close();
    });
    Backend backend(test_options());
    bool ready = false;
    std::int64_t confirmed = 0;
    const Error error = Access::http_upload(
        backend, server.url("/upload"), std::string(1024, '\0'), [&ready] { ready = true; },
        [&confirmed](std::int64_t count) { confirmed += count; });
    EXPECT_TRUE(error);
    EXPECT_FALSE(ready);
    EXPECT_EQ(confirmed, 0);
}

TEST(Yandex, WebSocketSchemaViolationIsProtocolError) {
    MockServer server([](MockExchange& exchange) {
        auto& socket = exchange.websocket();
        (void)socket.read();
        socket.write_text(R"({"k":"x","b":1})");
        (void)socket.read();
    });
    Backend backend(test_options());
    const Error error = Access::websocket_upload(
        backend, Context(), server.ws_url(), {}, [] {}, [](std::int64_t) {});
    EXPECT_EQ(service::classify_error(Error::wrap("upload stream", error)),
              service::ErrorCode::protocol);
}

std::string internet_page(std::string_view script) {
    return "<html><head></head><body><script>" + std::string(script) + "</script></body></html>";
}

Result<service::ConnectionInfo> detect(int status, std::string content_type, std::string body) {
    MockServer server(
        [&](MockExchange& exchange) {
            testing::Headers headers;
            if (!content_type.empty()) {
                headers.emplace_back("Content-Type", content_type);
            }
            exchange.respond(status, headers, body);
        },
        false);
    Options options = test_options();
    options.internet_page_url = server.url("/internet/");
    Backend backend(options);
    return backend.detect_connection(Context());
}

TEST(Yandex, DetectConnectionParsesIpv4) {
    const auto info = detect(
        200, "text/html; charset=utf-8",
        internet_page(
            R"(Client.default({"blackbox":{"isValid":false},"ip":{"v4":"203.0.113.7","v6":null},"other":{"nested":{"a":1}}}))"));
    ASSERT_TRUE(info) << info.error().message();
    EXPECT_EQ(info->external_ip.to_string(), "203.0.113.7");
    EXPECT_TRUE(info->isp.empty());
}

TEST(Yandex, DetectConnectionFallsBackToIpv6) {
    const auto info = detect(
        200, "text/html", internet_page(R"(Client.default({"ip":{"v4":"","v6":"2001:db8::1"}}))"));
    ASSERT_TRUE(info) << info.error().message();
    EXPECT_EQ(info->external_ip.to_string(), "2001:db8::1");
}

TEST(Yandex, DetectConnectionRejectsInvalidResponses) {
    const std::vector<std::pair<int, std::string>> cases = {
        {200, internet_page(R"(SomethingElse({"ip":{"v4":"203.0.113.7"}}))")},
        {200, internet_page(R"(Client.default({"ip":{"v4":"203.0.113.7")")},
        {200, internet_page(R"(Client.default({"blackbox":{"isValid":false}}))")},
        {200, internet_page(R"(Client.default({"ip":{"v4":"not-an-ip","v6":""}}))")},
        {200, internet_page(R"(Client.default({"ip":{"v4":"fe80::1%eth0"}}))")},
        {200, internet_page(R"(Client.default({"ip":{"v4":7}}))")},
        {500, internet_page(R"(Client.default({"ip":{"v4":"203.0.113.7"}}))")},
    };
    for (const auto& [status, body] : cases) {
        const auto info = detect(status, "text/html", body);
        EXPECT_FALSE(info) << body;
    }
    const auto server_error = detect(500, "text/html", "failed");
    ASSERT_FALSE(server_error);
    EXPECT_EQ(server_error.error().as<service::OpError>()->code, service::ErrorCode::unavailable);
}

TEST(Yandex, DetectConnectionRejectsOversizedPageAndWrongType) {
    EXPECT_FALSE(detect(200, "text/html", std::string((8 << 20) + 1, ' ')));
    EXPECT_FALSE(detect(200, "application/octet-stream",
                        internet_page(R"(Client.default({"ip":{"v4":"203.0.113.7"}}))")));
}

} // namespace
} // namespace puls::yandex
