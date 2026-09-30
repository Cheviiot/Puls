// Live tests contact the real measurement services and are built only with
// PULS_LIVE_TESTS=ON. Throughput runs only with PULS_LIVE_THROUGHPUT=1.

#include "puls/core/text.hpp"
#include "puls/service/speedtestru/backend.hpp"
#include "puls/service/yandex/backend.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace puls {
namespace {

using namespace std::chrono_literals;

bool throughput_enabled() {
    const char* value = std::getenv("PULS_LIVE_THROUGHPUT");
    return value != nullptr && std::string_view(value) == "1";
}

template <class Operation>
auto retry_twice(Operation operation) {
    auto result = operation();
    if (!result) {
        result = operation();
    }
    return result;
}

void run_throughput(service::Backend& backend, const Context& ctx) {
    if (!throughput_enabled()) {
        GTEST_SKIP() << "set PULS_LIVE_THROUGHPUT=1 to run download/upload";
    }
    const service::MeasurementConfig config{10s, 0, 16};
    const auto download = backend.download(ctx, config, nullptr);
    ASSERT_FALSE(download.error) << download.error.message();
    ASSERT_GT(download.result.bytes, 0);
    const auto upload = backend.upload(ctx, config, nullptr);
    ASSERT_FALSE(upload.error) << upload.error.message();
    ASSERT_GT(upload.result.bytes, 0);
    std::cout << "download=" << text::format_fixed(download.result.mbps, 2)
              << "Mbps upload=" << text::format_fixed(upload.result.mbps, 2) << "Mbps\n";
}

TEST(Live, Yandex) {
    const CancelScope scope(Context(), 90s);
    const Context& ctx = scope.context();
    yandex::Backend backend;
    const auto connection = backend.detect_connection(ctx);
    ASSERT_TRUE(connection) << connection.error().message();
    ASSERT_TRUE(connection->external_ip.valid());
    const auto server = retry_twice([&] { return backend.select_server(ctx); });
    ASSERT_TRUE(server) << server.error().message();
    const auto ping = retry_twice([&] { return backend.ping(ctx); });
    ASSERT_TRUE(ping) << ping.error().message();
    ASSERT_GT(ping->samples, 0);
    ASSERT_GT(ping->value_ms, 0);
    std::cout << "server=" << server->name << " ping=" << text::format_fixed(ping->value_ms, 2)
              << "ms\n";
    run_throughput(backend, ctx);
}

TEST(Live, Speedtest) {
    const CancelScope scope(Context(), 110s);
    const Context& ctx = scope.context();
    speedtestru::Backend backend;
    const auto connection = backend.detect_connection(ctx);
    ASSERT_TRUE(connection) << connection.error().message();
    ASSERT_TRUE(connection->external_ip.valid());
    ASSERT_FALSE(connection->isp.empty());
    const auto server = retry_twice([&] { return backend.select_server(ctx); });
    ASSERT_TRUE(server) << server.error().message();
    const auto ping = retry_twice([&] { return backend.ping(ctx); });
    ASSERT_TRUE(ping) << ping.error().message();
    ASSERT_EQ(ping->samples, 10);
    ASSERT_GT(ping->value_ms, 0);
    std::cout << "server=" << server->name << " ping=" << text::format_fixed(ping->value_ms, 2)
              << "ms jitter=" << text::format_fixed(ping->jitter_ms, 2) << "ms\n";
    run_throughput(backend, ctx);
}

} // namespace
} // namespace puls
