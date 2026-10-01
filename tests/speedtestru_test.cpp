#include "puls/core/text.hpp"
#include "puls/service/speedtestru/backend.hpp"

#include "support/mock_server.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace puls::speedtestru {

struct Backend::TestAccess {
    static void set_jwt(Backend& backend, std::string jwt) {
        std::lock_guard lock(backend.mutex_);
        backend.jwt_ = std::move(jwt);
    }
    static std::string jwt(Backend& backend) {
        std::lock_guard lock(backend.mutex_);
        return backend.jwt_;
    }
    static std::string browser_key(Backend& backend) { return backend.current_browser_key(); }
    static void set_servers(Backend& backend, const std::vector<std::string>& hosts,
                            bool throughput = true, Error discovery_error = {},
                            std::optional<service::PingResult> cached = std::nullopt) {
        std::lock_guard lock(backend.mutex_);
        backend.servers_.clear();
        for (const auto& host : hosts) {
            QmsServer server;
            server.host = host;
            server.name = host;
            backend.servers_.push_back(server);
        }
        backend.selected_ = backend.servers_.front();
        if (cached) {
            backend.selected_.ping = *cached;
        }
        backend.throughput_ = throughput;
        backend.discovery_error_ = std::move(discovery_error);
    }
    static std::string selected_host(Backend& backend) {
        std::lock_guard lock(backend.mutex_);
        return backend.selected_.host;
    }
    static std::vector<std::string> server_hosts(Backend& backend) {
        std::lock_guard lock(backend.mutex_);
        std::vector<std::string> hosts;
        for (const auto& server : backend.servers_) {
            hosts.push_back(server.host);
        }
        return hosts;
    }
    static Result<std::int64_t> download_request(Backend& backend, const std::string& host,
                                                 int chunk_mb, std::size_t buffer_size,
                                                 const measure::ReadyFn& ready,
                                                 const measure::RecordFn& record) {
        net::HttpSession session(backend.measurement_options());
        std::vector<char> buffer(buffer_size);
        QmsServer server;
        server.host = host;
        return backend.download_request(Context(), session, server, chunk_mb, buffer, ready,
                                        record);
    }
    static Error upload_request(Backend& backend, const std::string& host, std::string_view payload,
                                const measure::ReadyFn& ready) {
        net::HttpSession session(backend.measurement_options());
        QmsServer server;
        server.host = host;
        return backend.upload_request(Context(), session, server, payload, ready);
    }
    static Result<std::string> ensure_jwt(Backend& backend, bool refresh,
                                          const std::string& previous) {
        return backend.ensure_jwt(Context(), refresh, previous);
    }
    static Result<std::vector<std::string>> fetch_nearest(Backend& backend,
                                                          const std::string& key) {
        auto fetched = backend.fetch_nearest(Context(), key);
        if (fetched.error) {
            return fetched.error;
        }
        std::vector<std::string> hosts;
        for (const auto& server : fetched.value) {
            hosts.push_back(server.host + "|" + server.city);
        }
        return hosts;
    }
    static Result<std::string> extract_browser_key(Backend& backend) {
        return backend.extract_browser_key(Context());
    }
    static Result<service::PingResult> probe_ping(Backend& backend, const std::string& host,
                                                  int samples) {
        QmsServer server;
        server.host = host;
        return backend.probe_ping(Context(), server, samples);
    }
};

namespace {

using namespace std::chrono_literals;
using Access = Backend::TestAccess;
using testing::MockExchange;
using testing::MockServer;

const testing::Headers json_type = {{"Content-Type", "application/json"}};
const testing::Headers html_type = {{"Content-Type", "text/html; charset=utf-8"}};
const testing::Headers script_type = {{"Content-Type", "text/plain; charset=utf-8"}};

Options test_options(std::string api_base = {}) {
    Options options;
    options.tls = testing::test_tls_context();
    if (!api_base.empty()) {
        options.api_base = std::move(api_base);
    }
    return options;
}

// Serves the QMS text protocol on every WebSocket upgrade.
void serve_qms(MockExchange& exchange, std::string_view server_info = "qms_testing/1.0") {
    auto& socket = exchange.websocket();
    while (auto message = socket.read()) {
        if (!message->text) {
            return;
        }
        if (message->payload == "HI") {
            socket.write_text("HELLO");
        } else if (message->payload == "GETINFO") {
            socket.write_text(server_info);
        } else if (message->payload == "AUTH") {
            socket.write_text("READY_TO_TEST");
        } else if (message->payload == "PING") {
            socket.write_text("PONG 123");
        }
    }
}

TEST(SpeedtestProtocol, NormalizeHost) {
    struct Case {
        const char* input;
        const char* want;
    };
    for (const Case& test :
         {Case{"qms.example", "qms.example:20000"}, Case{"qms.example:443", "qms.example:443"},
          Case{"127.0.0.1", "127.0.0.1:20000"}, Case{"::1", "[::1]:20000"},
          Case{"[::1]:443", "[::1]:443"}, Case{"[::1]", "[::1]:20000"},
          Case{" qms.example ", "qms.example:20000"}}) {
        const auto host = detail::normalize_host(test.input, "20000");
        ASSERT_TRUE(host) << test.input << ": " << host.error().message();
        EXPECT_EQ(*host, test.want) << test.input;
    }
    for (const char* invalid : {"https://qms.example", "qms.example/path", "qms.example:0",
                                "qms.example:70000", "a:b:c", "", "user@qms.example"}) {
        EXPECT_FALSE(detail::normalize_host(invalid, "20000")) << invalid;
    }
    EXPECT_EQ(detail::normalize_host("a:b:c", "20000").error().message(),
              "неверный адрес сервера \"a:b:c\": address a:b:c: too many colons in address");
}

TEST(SpeedtestProtocol, NativeCalculationsAndReconnectRotation) {
    EXPECT_EQ(detail::qms_jitter({10, 40, 20, 30, 50, 60, 70, 80, 90, 100}, 45), 10);
    EXPECT_EQ(detail::qms_jitter({10}, 10), 0);
    EXPECT_EQ(detail::qms_jitter({10, 10, 10, 10}, 10), 1);
    EXPECT_EQ(detail::next_download_chunk_mb(0, 1s), 25);
    EXPECT_EQ(detail::next_download_chunk_mb(25'000'000, 6s), 25);
    EXPECT_EQ(detail::next_download_chunk_mb(100'000'000, 6s), 100);
    EXPECT_EQ(detail::next_download_chunk_mb(1'000'000'000, 1s), 250);
    std::atomic<std::uint32_t> attempts{0};
    EXPECT_EQ(detail::next_server_index(3, 1, attempts), 1u);
    EXPECT_EQ(detail::next_server_index(3, 1, attempts), 2u);
    ASSERT_FALSE(detail::known_servers().empty());
    for (const auto& server : detail::known_servers()) {
        EXPECT_FALSE(server.host.empty());
    }
}

TEST(SpeedtestProtocol, FindsScriptsAndBrowserKey) {
    EXPECT_EQ(
        detail::script_sources(
            R"(<script src="/app/page-test.js"></script><img src="x.png"><script src='//cdn/x.js?v=1'></script>)",
            32),
        (std::vector<std::string>{"/app/page-test.js", "//cdn/x.js?v=1"}));
    EXPECT_TRUE(detail::script_sources(R"(<script src=".js"></script>)", 32).empty());
    EXPECT_EQ(detail::qms_library_key(R"(new QMSLibrary({ id : "abcdefghijklmnop_-123" }))"),
              "abcdefghijklmnop_-123");
    EXPECT_EQ(detail::qms_library_key(
                  R"(QMSLibrary({id:'short'}) QMSLibrary({id:'longer_key_value_1'}))"),
              "longer_key_value_1");
    EXPECT_FALSE(detail::qms_library_key("QMSLibrary({id:\"" + std::string(129, 'a') + "\"})"));
    EXPECT_FALSE(detail::qms_library_key("QMSLibrary ({id:\"abcdefghijklmnopq\"})"));
}

TEST(Speedtest, SelectServerDefaultsAndKeepsPorts) {
    Options options = test_options();
    options.server = "vladivostok.qms.ru";
    Backend backend(options);
    const auto server = backend.select_server(Context());
    ASSERT_TRUE(server) << server.error().message();
    EXPECT_EQ(server->name, "vladivostok.qms.ru:20000");

    options.server = "vladivostok.qms.ru:12345";
    Backend explicit_port(options);
    EXPECT_EQ(explicit_port.select_server(Context())->name, "vladivostok.qms.ru:12345");

    options.server = "https://bad.example";
    Backend invalid(options);
    const auto failed = invalid.select_server(Context());
    ASSERT_FALSE(failed);
    EXPECT_EQ(failed.error().as<service::OpError>()->code, service::ErrorCode::internal);
}

TEST(Speedtest, AutoSelectFailsFastWithCanceledContext) {
    Backend backend(test_options());
    CancelScope scope{Context()};
    scope.cancel();
    const auto server = backend.select_server(scope.context());
    ASSERT_FALSE(server);
    EXPECT_TRUE(server.error().is(errors::canceled_tag));
    const auto* operation = server.error().as<service::OpError>();
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->code, service::ErrorCode::canceled);
    EXPECT_FALSE(operation->retryable);
}

TEST(Speedtest, PhasesRequireSelectedServer) {
    Options options = test_options();
    options.server = "example.com";
    Backend backend(options);
    EXPECT_FALSE(backend.ping(Context()));
    const service::MeasurementConfig config{3s, 0, 0};
    EXPECT_TRUE(backend.download(Context(), config, nullptr).error);
    EXPECT_TRUE(backend.upload(Context(), config, nullptr).error);
    const auto capabilities = backend.capabilities();
    EXPECT_TRUE(service::has(capabilities, service::Capability::ping));
    EXPECT_TRUE(service::has(capabilities, service::Capability::download));
    EXPECT_TRUE(service::has(capabilities, service::Capability::upload));
}

TEST(Speedtest, QmsPingDownloadUploadProtocol) {
    const std::string token = "test-jwt-token-long-enough";
    std::atomic<int> upload_size{0};
    MockServer server([&](MockExchange& exchange) {
        const auto& request = exchange.request();
        if (request.websocket_upgrade) {
            serve_qms(exchange, R"({"name":"qms_testing"})");
            return;
        }
        if (request.path == "/download.php") {
            if (request.header("jwt") != token || request.header("Accept-Encoding") != "identity" ||
                request.query_value("ckSize") != "1") {
                exchange.respond(401, {}, "bad auth or compression");
                return;
            }
            exchange.respond_chunked(200, {}, std::string(1'000'000, 'd'));
            return;
        }
        if (request.path == "/upload.php") {
            if (request.header("jwt") != token) {
                exchange.respond(401, {}, "bad auth");
                return;
            }
            upload_size.store(static_cast<int>(request.body.size()));
            exchange.respond(204);
            return;
        }
        exchange.respond(404);
    });
    Backend backend(test_options());
    Access::set_jwt(backend, token);

    const auto ping = Access::probe_ping(backend, server.host(), 3);
    ASSERT_TRUE(ping) << ping.error().message();
    EXPECT_EQ(ping->samples, 3);
    EXPECT_EQ(ping->method, "median");

    bool ready = false;
    std::int64_t recorded = 0;
    const auto bytes = Access::download_request(
        backend, server.host(), 1, 64 << 10, [&ready] { ready = true; },
        [&recorded](std::int64_t count) { recorded += count; });
    ASSERT_TRUE(bytes) << bytes.error().message();
    EXPECT_EQ(*bytes, 1'000'000);
    EXPECT_EQ(recorded, 1'000'000);
    EXPECT_TRUE(ready);

    bool upload_ready = false;
    EXPECT_FALSE(Access::upload_request(backend, server.host(), std::string(2 << 20, '\0'),
                                        [&upload_ready] { upload_ready = true; }));
    EXPECT_TRUE(upload_ready);
    EXPECT_EQ(upload_size.load(), 2 << 20);
}

TEST(Speedtest, QmsHandshakeRejectsUnexpectedServer) {
    MockServer server([](MockExchange& exchange) { serve_qms(exchange, "nginx"); });
    Backend backend(test_options());
    const auto ping = Access::probe_ping(backend, server.host(), 1);
    ASSERT_FALSE(ping);
    EXPECT_EQ(service::classify_error(ping.error()), service::ErrorCode::protocol);
}

TEST(Speedtest, PingUsesTenCachedDiscoverySamples) {
    const std::vector<double> samples{10, 11, 9, 10, 12, 10, 11, 9, 10, 10};
    service::PingResult cached = service::stats_with_method(samples, "median");
    cached.jitter_ms = detail::qms_jitter(samples, cached.median_ms);
    Backend backend(test_options());
    Access::set_servers(backend, {"unreachable.invalid:20000"}, true, {}, cached);
    const auto result = backend.ping(Context());
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(*result, cached);
}

TEST(Speedtest, PingFailsOverToNextResponsiveServer) {
    MockServer server([](MockExchange& exchange) { serve_qms(exchange); });
    std::string refused;
    {
        MockServer placeholder([](MockExchange&) {});
        refused = placeholder.host();
    }
    Backend backend(test_options());
    Access::set_servers(backend, {refused, server.host()});
    const auto result = backend.ping(Context());
    ASSERT_TRUE(result) << result.error().message();
    EXPECT_EQ(result->samples, 10);
    EXPECT_EQ(Access::selected_host(backend), server.host());
    EXPECT_EQ(Access::server_hosts(backend), std::vector<std::string>{server.host()});
}

TEST(Speedtest, DownloadRefreshesJwtAfterUnauthorized) {
    const std::string fresh = "fresh.payload.signature";
    std::atomic<int> token_calls{0};
    MockServer server([&](MockExchange& exchange) {
        const auto& request = exchange.request();
        if (request.path == "/api/server/gentoken") {
            token_calls.fetch_add(1);
            exchange.respond(200, json_type, R"({"token":")" + fresh + R"("})");
        } else if (request.header("jwt") != fresh) {
            exchange.respond(401);
        } else {
            exchange.respond(200, {}, std::string(1'000'000, 'd'));
        }
    });
    Options options = test_options(server.url());
    options.browser_key = "browser-key-long-enough";
    Backend backend(options);
    Access::set_jwt(backend, "stale-jwt-token-long-enough");
    std::int64_t recorded = 0;
    const auto bytes = Access::download_request(
        backend, server.host(), 1, 64 << 10, [] {},
        [&recorded](std::int64_t count) { recorded += count; });
    ASSERT_TRUE(bytes) << bytes.error().message();
    EXPECT_EQ(*bytes, 1'000'000);
    EXPECT_EQ(recorded, 1'000'000);
    EXPECT_EQ(token_calls.load(), 1);
    EXPECT_EQ(Access::jwt(backend), fresh);
}

TEST(Speedtest, EnsureJwtRotatesBrowserKey) {
    const std::string new_key = "new-browser-key-123456789";
    const std::string token = "rotated.payload.signature";
    std::atomic<int> old_key_calls{0};
    MockServer server(
        [&](MockExchange& exchange) {
            const auto& request = exchange.request();
            if (request.path == "/api/server/gentoken") {
                if (request.header("x-api-key") != new_key) {
                    old_key_calls.fetch_add(1);
                    exchange.respond(403);
                    return;
                }
                exchange.respond(200, json_type, R"({"token":")" + token + R"("})");
            } else if (request.path == "/") {
                exchange.respond(200, html_type, R"(<script src="/app/page-test.js"></script>)");
            } else if (request.path == "/app/page-test.js") {
                exchange.respond(200, script_type, R"(new QMSLibrary({id:")" + new_key + R"("}))");
            } else {
                exchange.respond(404);
            }
        },
        false);
    Options options = test_options(server.url());
    options.browser_key = "obsolete-browser-key-123";
    Backend backend(options);
    const auto jwt = Access::ensure_jwt(backend, false, {});
    ASSERT_TRUE(jwt) << jwt.error().message();
    EXPECT_EQ(*jwt, token);
    EXPECT_EQ(Access::browser_key(backend), new_key);
    EXPECT_EQ(old_key_calls.load(), 1);
}

TEST(Speedtest, FetchNearestValidatesCandidates) {
    MockServer server(
        [](MockExchange& exchange) {
            if (exchange.request().header("x-api-key") != "key") {
                exchange.respond(403);
                return;
            }
            exchange.respond(
                200, json_type,
                R"({"algorithm":"nearest","data":[)"
                R"({"id":1,"name":"moscow.speedtest.ru","city":"Москва","src":"https://moscow.qms.ru","source":"provider","port":20000},)"
                R"({"id":2,"name":"bad","city":"Москва","src":"http://insecure.invalid","source":"provider","port":20000},)"
                R"({"id":3,"name":"dup","city":"Москва","src":"https://moscow.qms.ru/","source":"provider","port":20000},)"
                R"({"id":4,"name":"path","city":"Москва","src":"https://path.qms.ru/x","source":"provider","port":20000},)"
                R"({"id":0,"name":"id","city":"Москва","src":"https://id.qms.ru","source":"provider","port":20000}]})");
        },
        false);
    Backend backend(test_options(server.url()));
    const auto servers = Access::fetch_nearest(backend, "key");
    ASSERT_TRUE(servers) << servers.error().message();
    EXPECT_EQ(*servers, std::vector<std::string>{"moscow.qms.ru:20000|Москва"});
    EXPECT_FALSE(Access::fetch_nearest(backend, "wrong"));
}

TEST(Speedtest, SelectServerDiscoversAndRanksResponsiveServers) {
    std::atomic<int> nearest_calls{0};
    MockServer qms([](MockExchange& exchange) {
        if (exchange.request().websocket_upgrade) {
            serve_qms(exchange);
        }
    });
    MockServer api(
        [&](MockExchange& exchange) {
            nearest_calls.fetch_add(1);
            exchange.respond(
                200, json_type,
                R"({"algorithm":"nearest","data":[{"id":7,"name":"local","city":"Тест","src":"https://127.0.0.1","source":"provider","port":)" +
                    std::to_string(qms.port()) + R"(,"region_name":"Регион"}]})");
        },
        false);
    std::vector<std::string> logs;
    Options options = test_options(api.url());
    options.log = [&logs](std::string_view message) { logs.emplace_back(message); };
    Backend backend(options);
    const auto server = backend.select_server(Context());
    ASSERT_TRUE(server) << server.error().message();
    EXPECT_EQ(server->name, qms.host());
    EXPECT_EQ(server->city, "Тест");
    EXPECT_EQ(server->region, "Регион");
    EXPECT_EQ(nearest_calls.load(), 1);
    ASSERT_FALSE(logs.empty());
    EXPECT_TRUE(text::contains(logs.back(), "выбран=" + qms.host()));
    const auto ping = backend.ping(Context());
    ASSERT_TRUE(ping) << ping.error().message();
    EXPECT_EQ(ping->samples, 10);
}

TEST(Speedtest, FallbackServerIsPingOnly) {
    Backend backend(test_options());
    Access::set_servers(backend, {"fallback.qms.ru:20000"}, false,
                        Error::make("discovery unavailable"));
    const auto outcome = backend.download(Context(), service::MeasurementConfig{3s, 0, 0}, nullptr);
    ASSERT_TRUE(outcome.error);
    const auto* operation = outcome.error.as<service::OpError>();
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->code, service::ErrorCode::auth);
    EXPECT_TRUE(operation->retryable);
    EXPECT_TRUE(detail::is_ping_only_fallback(outcome.error));
}

TEST(Speedtest, DownloadValidatesResponseBeforeCounting) {
    MockServer server([](MockExchange& exchange) {
        if (exchange.request().query_value("ckSize") != "1") {
            exchange.respond(400);
        } else if (exchange.request().path == "/length/download.php") {
            exchange.respond(200, {}, "1234567");
        } else {
            exchange.respond(200, {{"Content-Encoding", "gzip"}}, "compressed");
        }
    });
    Backend backend(test_options());
    Access::set_jwt(backend, "cached.payload.signature");
    for (const std::string prefix : {"/length", "/gzip"}) {
        bool ready = false;
        std::int64_t recorded = 0;
        const auto bytes = Access::download_request(
            backend, server.host() + prefix, 1, 4096, [&ready] { ready = true; },
            [&recorded](std::int64_t count) { recorded += count; });
        ASSERT_FALSE(bytes) << prefix;
        EXPECT_EQ(service::classify_error(bytes.error()), service::ErrorCode::protocol) << prefix;
        EXPECT_FALSE(ready) << prefix;
        EXPECT_EQ(recorded, 0) << prefix;
    }
}

TEST(Speedtest, DownloadRejectsTruncatedAndOversizedPayloads) {
    MockServer server([](MockExchange& exchange) {
        if (exchange.request().path == "/truncated/download.php") {
            exchange.respond(200, {{"Content-Length", "1000000"}}, std::string(1024, 'x'));
            exchange.close();
            return;
        }
        exchange.respond_chunked(200, {}, std::string(1'000'001, 'x'));
    });
    Backend backend(test_options());
    Access::set_jwt(backend, "cached.payload.signature");
    std::int64_t recorded = 0;
    auto bytes = Access::download_request(
        backend, server.host() + "/truncated", 1, 4096, [] {},
        [&recorded](std::int64_t count) { recorded += count; });
    EXPECT_FALSE(bytes);
    EXPECT_EQ(recorded, 1024);
    bytes = Access::download_request(
        backend, server.host() + "/oversized", 1, 64 << 10, [] {}, [](std::int64_t) {});
    ASSERT_FALSE(bytes);
    EXPECT_EQ(service::classify_error(bytes.error()), service::ErrorCode::protocol);
}

TEST(Speedtest, ThroughputAuthIsRefreshedOnlyOnce) {
    const std::string fresh = "fresh.payload.signature";
    for (const bool download : {true, false}) {
        std::atomic<int> token_calls{0};
        std::atomic<int> transfer_calls{0};
        MockServer server([&](MockExchange& exchange) {
            if (exchange.request().path == "/api/server/gentoken") {
                token_calls.fetch_add(1);
                exchange.respond(200, json_type, R"({"token":")" + fresh + R"("})");
                return;
            }
            transfer_calls.fetch_add(1);
            exchange.respond(401);
        });
        Options options = test_options(server.url());
        options.browser_key = "browser-key-long-enough";
        Backend backend(options);
        Access::set_jwt(backend, "stale.payload.signature");
        Error error;
        if (download) {
            error = Access::download_request(
                        backend, server.host(), 1, 4096, [] {}, [](std::int64_t) {})
                        .error();
        } else {
            error =
                Access::upload_request(backend, server.host(), std::string(2 << 20, '\0'), [] {});
        }
        ASSERT_TRUE(error);
        EXPECT_EQ(service::classify_error(error), service::ErrorCode::auth);
        EXPECT_EQ(token_calls.load(), 1);
        EXPECT_EQ(transfer_calls.load(), 2);
    }
}

TEST(Speedtest, UploadDoesNotConfirmFailedResponseOrRedirect) {
    MockServer server([](MockExchange& exchange) {
        if (exchange.request().path == "/redirect/upload.php") {
            exchange.respond(302, {{"Location", "/elsewhere"}});
            return;
        }
        exchange.respond(500, {}, "failed");
    });
    Backend backend(test_options());
    Access::set_jwt(backend, "cached.payload.signature");
    for (const std::string prefix : {"", "/redirect"}) {
        bool ready = false;
        EXPECT_TRUE(Access::upload_request(backend, server.host() + prefix,
                                           std::string(2 << 20, '\0'), [&ready] { ready = true; }));
        EXPECT_FALSE(ready);
    }
    EXPECT_TRUE(Access::upload_request(backend, server.host(), "short", [] {}));
}

TEST(Speedtest, ExtractBrowserKeyRejectsCrossOriginAssets) {
    std::atomic<int> external_calls{0};
    MockServer external(
        [&external_calls](MockExchange& exchange) {
            external_calls.fetch_add(1);
            exchange.respond(200, script_type,
                             R"(new QMSLibrary({id:"external-browser-key-123"}))");
        },
        false);
    MockServer page(
        [&external](MockExchange& exchange) {
            exchange.respond(200, html_type,
                             R"(<script src=")" + external.url("/bundle.js") + R"("></script>)");
        },
        false);
    Backend backend(test_options(page.url()));
    EXPECT_FALSE(Access::extract_browser_key(backend));
    EXPECT_EQ(external_calls.load(), 0);
}

TEST(Speedtest, ExtractBrowserKeyFollowsSameOriginRedirects) {
    MockServer server(
        [](MockExchange& exchange) {
            const auto& path = exchange.request().path;
            if (path == "/") {
                exchange.respond(200, html_type, R"(<script src="/old.js"></script>)");
            } else if (path == "/old.js") {
                exchange.respond(301, {{"Location", "/static/new.js"}});
            } else {
                exchange.respond(200, {{"Content-Type", "application/javascript"}},
                                 R"(QMSLibrary({id:"redirected-key-12345"}))");
            }
        },
        false);
    Backend backend(test_options(server.url()));
    const auto key = Access::extract_browser_key(backend);
    ASSERT_TRUE(key) << key.error().message();
    EXPECT_EQ(*key, "redirected-key-12345");
}

Result<service::ConnectionInfo> detect(MockServer::Handler handler, Options options = {}) {
    MockServer server(std::move(handler), false);
    options.tls = testing::test_tls_context();
    options.api_base = server.url();
    Backend backend(options);
    return backend.detect_connection(Context());
}

TEST(Speedtest, DetectConnectionReturnsIpAndIsp) {
    Options options;
    options.browser_key = "test-browser-key";
    const auto info = detect(
        [](MockExchange& exchange) {
            const auto& request = exchange.request();
            if (request.path == "/api/asn_provider/ip") {
                if (request.header("x-api-key") != "test-browser-key") {
                    exchange.respond(401, {}, "missing key");
                    return;
                }
                exchange.respond(200, {{"Content-Type", "application/json; charset=utf-8"}},
                                 R"({"ip":"2001:db8::7"})");
                return;
            }
            exchange.respond(200, json_type,
                             R"({"ip":"2001:db8::7","provider_name":"Тест Телеком"})");
        },
        options);
    ASSERT_TRUE(info) << info.error().message();
    EXPECT_EQ(info->external_ip.to_string(), "2001:db8::7");
    EXPECT_EQ(info->isp, "Тест Телеком");
    EXPECT_TRUE(info->warnings.empty());
}

TEST(Speedtest, DetectConnectionKeepsIpWhenIspIsUnavailable) {
    const auto info = detect([](MockExchange& exchange) {
        if (exchange.request().path == "/api/asn_provider/ip") {
            exchange.respond(200, json_type, R"({"ip":"203.0.113.9"})");
            return;
        }
        exchange.respond(503, {}, "unavailable");
    });
    ASSERT_TRUE(info) << info.error().message();
    EXPECT_EQ(info->external_ip.to_string(), "203.0.113.9");
    EXPECT_TRUE(info->isp.empty());
    ASSERT_EQ(info->warnings.size(), 1u);
    EXPECT_EQ(info->warnings[0].rfind("интернет-провайдер недоступен: ", 0), 0u);
}

TEST(Speedtest, DetectConnectionRejectsInvalidIpResponses) {
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"application/json", R"({"ip":)"},
        {"text/plain", R"({"ip":"203.0.113.9"})"},
        {"application/json", R"({"ip":"not-an-ip"})"},
        {"application/json", R"({"ip":"fe80::1%eth0"})"},
        {"application/json", std::string((64 << 10) + 1, ' ')},
    };
    for (const auto& [type, body] : cases) {
        const auto info = detect([&type, &body](MockExchange& exchange) {
            exchange.respond(200, {{"Content-Type", type}}, body);
        });
        ASSERT_FALSE(info) << body.substr(0, 40);
        EXPECT_EQ(info.error().as<service::OpError>()->code, service::ErrorCode::protocol);
    }
}

TEST(Speedtest, DetectConnectionRotatesBrowserKeyOnce) {
    const std::string new_key = "new-connection-key-123456";
    std::atomic<int> rejected{0};
    Options options;
    options.browser_key = "obsolete-key-123456789";
    MockServer server(
        [&](MockExchange& exchange) {
            const auto& request = exchange.request();
            if (request.path == "/api/asn_provider/ip") {
                if (request.header("x-api-key") != new_key) {
                    rejected.fetch_add(1);
                    exchange.respond(403);
                    return;
                }
                exchange.respond(200, json_type, R"({"ip":"203.0.113.10"})");
            } else if (request.path == "/api/asn_provider/asn") {
                exchange.respond(200, json_type, R"({"ip":"203.0.113.10","provider_name":"ISP"})");
            } else if (request.path == "/") {
                exchange.respond(200, html_type, R"(<script src="/bundle.js"></script>)");
            } else {
                exchange.respond(200, script_type, R"(new QMSLibrary({id:")" + new_key + R"("}))");
            }
        },
        false);
    options.tls = testing::test_tls_context();
    options.api_base = server.url();
    Backend backend(options);
    const auto info = backend.detect_connection(Context());
    ASSERT_TRUE(info) << info.error().message();
    EXPECT_EQ(info->isp, "ISP");
    EXPECT_EQ(rejected.load(), 1);
    EXPECT_EQ(Access::browser_key(backend), new_key);
}

TEST(Speedtest, DetectConnectionIgnoresIspFromMismatchedOrMissingName) {
    for (const std::string asn : {R"({"ip":"203.0.113.2","provider_name":"Wrong ISP"})",
                                  R"({"ip":"203.0.113.1","provider_name":"  "})",
                                  R"({"ip":"203.0.113.1","provider_name":"line\nbreak"})"}) {
        const auto info = detect([&asn](MockExchange& exchange) {
            if (exchange.request().path == "/api/asn_provider/ip") {
                exchange.respond(200, json_type, R"({"ip":"203.0.113.1"})");
                return;
            }
            exchange.respond(200, json_type, asn);
        });
        ASSERT_TRUE(info) << info.error().message();
        EXPECT_TRUE(info->isp.empty()) << asn;
        EXPECT_EQ(info->warnings.size(), 1u) << asn;
    }
}

TEST(Speedtest, DetectConnectionClassifiesIpEndpoint5xx) {
    const auto info =
        detect([](MockExchange& exchange) { exchange.respond(503, {}, "unavailable"); });
    ASSERT_FALSE(info);
    const auto* operation = info.error().as<service::OpError>();
    ASSERT_NE(operation, nullptr);
    EXPECT_EQ(operation->code, service::ErrorCode::unavailable);
    EXPECT_TRUE(operation->retryable);
}

TEST(Speedtest, DetectConnectionCancellation) {
    CancelScope scope{Context()};
    scope.cancel();
    Backend backend(test_options());
    const auto info = backend.detect_connection(scope.context());
    ASSERT_FALSE(info);
    EXPECT_TRUE(info.error().is(errors::canceled_tag));
    EXPECT_EQ(info.error().as<service::OpError>()->code, service::ErrorCode::canceled);
}

TEST(Speedtest, DetectConnectionRotatesBrowserKeyOnlyOnceAfterRepeatedRejection) {
    for (const int status : {401, 403}) {
        std::atomic<int> api_calls{0};
        std::atomic<int> page_calls{0};
        const auto info = detect([&](MockExchange& exchange) {
            const auto& path = exchange.request().path;
            if (path == "/api/asn_provider/ip") {
                api_calls.fetch_add(1);
                exchange.respond(status);
            } else if (path == "/") {
                page_calls.fetch_add(1);
                exchange.respond(200, html_type, R"(<script src="/bundle.js"></script>)");
            } else {
                exchange.respond(200, script_type, R"(new QMSLibrary({id:"still-rejected-key"}))");
            }
        });
        EXPECT_FALSE(info) << status;
        EXPECT_EQ(api_calls.load(), 2) << status;
        EXPECT_EQ(page_calls.load(), 1) << status;
    }
}

} // namespace
} // namespace puls::speedtestru
