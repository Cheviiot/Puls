#include "puls/core/text.hpp"
#include "puls/service/http.hpp"
#include "puls/service/service.hpp"

#include "support/mock_server.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace puls::service {
namespace {

using namespace std::chrono_literals;
using testing::MockExchange;
using testing::MockServer;

TEST(Stats, EmptyAndSingleSample) {
    EXPECT_EQ(stats({}), PingResult{});
    const PingResult expected{42, 42, 42, 42, 0, 1, "average"};
    EXPECT_EQ(stats({42}), expected);
}

TEST(Stats, KnownValues) {
    // min=10, avg=15, jitter = mean(|20-10|, |15-20|) = 7.5
    const PingResult expected{15, 10, 15, 15, 7.5, 3, "average"};
    EXPECT_EQ(stats({10, 20, 15}), expected);
}

TEST(Stats, OrderIndependentMinimumAndStableInput) {
    const std::vector<double> samples{30, 5, 20};
    const PingResult result = stats(samples);
    EXPECT_EQ(result.min_ms, 5);
    EXPECT_DOUBLE_EQ(result.avg_ms, (30 + 5 + 20) / 3.0);
    EXPECT_EQ(samples, (std::vector<double>{30, 5, 20}));
}

TEST(Stats, IgnoresInvalidSamples) {
    const PingResult expected{15, 10, 15, 15, 7.5, 3, "average"};
    EXPECT_EQ(stats({10, std::nan(""), -1, 20, std::numeric_limits<double>::infinity(), 15}),
              expected);
    EXPECT_EQ(
        stats_with_method({std::nan(""), -1, std::numeric_limits<double>::infinity()}, "minimum"),
        PingResult{});
}

TEST(Stats, FiniteForLargeSamples) {
    const double maximum = std::numeric_limits<double>::max();
    const PingResult result = stats({maximum, maximum});
    EXPECT_EQ(result.avg_ms, maximum);
    EXPECT_EQ(result.median_ms, maximum);
}

TEST(Stats, NativeMethods) {
    const PingResult minimum = stats_with_method({10, 12, 11}, "minimum");
    EXPECT_EQ(minimum.method, "minimum");
    EXPECT_EQ(minimum.value_ms, 10);
    const PingResult median = stats_with_method({10, 12, 11}, "median");
    EXPECT_EQ(median.method, "median");
    EXPECT_EQ(median.value_ms, 11);
    EXPECT_EQ(median.jitter_ms, 1.5);
    EXPECT_EQ(stats_with_method({10, 12}, "other").method, "average");
}

TEST(Stats, MedianAbsoluteDeviationIsRobust) {
    EXPECT_NEAR(median_absolute_deviation({10, 10.2, 80}), 0.2, 0.01);
    EXPECT_NEAR(median_absolute_deviation(
                    {10, std::nan(""), -3, 10.2, std::numeric_limits<double>::infinity()}),
                0.1, 0.01);
    EXPECT_EQ(median_absolute_deviation({5}), 0);
}

TEST(Errors, NewErrorPreservesDecisionAndCause) {
    const Error cause = Error::make("bad response");
    const Error error =
        new_error(ServiceId::yandex, Phase::ping, ErrorCode::protocol, false, cause);
    const auto* operation = error.as<OpError>();
    ASSERT_NE(operation, nullptr);
    EXPECT_FALSE(operation->retryable);
    EXPECT_TRUE(error.is(cause));
    EXPECT_EQ(error.message(), "Яндекс: задержка: bad response");
    EXPECT_EQ(new_error(ServiceId::speedtest, Phase::upload, ErrorCode::auth, true, {}).message(),
              "speedtest.ru: отдача: auth");
}

TEST(Errors, CancellationIsNeverRetryable) {
    for (const Error& error :
         {new_error(ServiceId::yandex, Phase::ping, ErrorCode::canceled, true, Error::make("x")),
          new_error(ServiceId::yandex, Phase::ping, ErrorCode::unavailable, true,
                    errors::canceled())}) {
        ASSERT_NE(error.as<OpError>(), nullptr);
        EXPECT_FALSE(error.as<OpError>()->retryable);
    }
}

TEST(Errors, TypedClassification) {
    EXPECT_EQ(classify_error(errors::canceled()), ErrorCode::canceled);
    EXPECT_EQ(classify_error(errors::deadline_exceeded()), ErrorCode::timeout);
    EXPECT_EQ(classify_error(protocol_error("bad frame")), ErrorCode::protocol);
    EXPECT_EQ(classify_error(authorization_error("bad token")), ErrorCode::auth);
    EXPECT_EQ(classify_error(http_status_error(403, "403 Forbidden", "gentoken")), ErrorCode::auth);
    EXPECT_EQ(classify_error(http_status_error(503, "503 Service Unavailable", "x")),
              ErrorCode::unavailable);
    EXPECT_EQ(classify_error(http_status_error(404, "404 Not Found", "x")), ErrorCode::protocol);
    EXPECT_EQ(classify_error(Error::with_detail(std::make_shared<NetworkError>(true), "timeout")),
              ErrorCode::timeout);
    EXPECT_EQ(classify_error(Error::make("connection reset")), ErrorCode::unavailable);
    const Error cause = Error::make("bad frame");
    EXPECT_TRUE(protocol_error(cause).is(cause));
    EXPECT_EQ(protocol_error(cause).message(), "ошибка протокола: bad frame");
    EXPECT_EQ(http_status_error(403, "403 Forbidden", "gentoken").message(),
              "gentoken вернул состояние 403 Forbidden");
    EXPECT_TRUE(retryable_code(ErrorCode::timeout));
    EXPECT_TRUE(retryable_code(ErrorCode::auth));
    EXPECT_FALSE(retryable_code(ErrorCode::protocol));
    EXPECT_EQ(classify_http_status(408), std::pair(ErrorCode::unavailable, true));
    EXPECT_EQ(classify_http_status(429), std::pair(ErrorCode::unavailable, true));
    EXPECT_EQ(classify_http_status(400), std::pair(ErrorCode::protocol, false));
}

TEST(Service, CapabilitiesAndNames) {
    const Capability capabilities = Capability::ping | Capability::download;
    EXPECT_TRUE(has(capabilities, Capability::ping));
    EXPECT_TRUE(has(capabilities, Capability::download));
    EXPECT_FALSE(has(capabilities, Capability::upload));
    EXPECT_EQ(parse_service_id(" YANDEX "), ServiceId::yandex);
    EXPECT_FALSE(parse_service_id("ip").has_value());
    EXPECT_EQ(display_name(ServiceId::yandex), "Яндекс");
    EXPECT_EQ(display_name(ServiceId::speedtest), "speedtest.ru");
    EXPECT_EQ(phase_display_name(Phase::download), "скачивание");
}

TEST(Service, ConvertsRunResultWithoutLeakingDiagnostics) {
    measure::RunResult result;
    result.bytes = 1'000'000;
    result.elapsed = 1s;
    result.workers_ok = 2;
    result.workers_failed = 1;
    result.worker_errors = {Error::make("same"), Error::make("same")};
    const ThroughputResult converted = convert_run_result(result);
    EXPECT_EQ(converted.mbps, 8);
    EXPECT_EQ(converted.successful_connections, 2);
    EXPECT_EQ(converted.failed_connections, 1);
    ASSERT_EQ(converted.warnings.size(), 1u);
    EXPECT_EQ(converted.warnings[0], "сетевые сбои отдельных потоков: 2");
}

TEST(Service, ConnectionLimits) {
    auto limits = connection_limits(MeasurementConfig{10s, 0, 16}, 20);
    ASSERT_TRUE(limits);
    EXPECT_EQ(*limits, std::pair(16, 16));
    limits = connection_limits(MeasurementConfig{10s, 0, 0}, 1);
    ASSERT_TRUE(limits);
    EXPECT_EQ(*limits, std::pair(1, 16));
    EXPECT_FALSE(connection_limits(MeasurementConfig{2s, 0, 16}, 4));
    EXPECT_FALSE(connection_limits(MeasurementConfig{61s, 0, 16}, 4));
    EXPECT_FALSE(connection_limits(MeasurementConfig{10s, 17, 16}, 4));
    EXPECT_FALSE(connection_limits(MeasurementConfig{10s, 0, 17}, 4));
    EXPECT_EQ(adaptive_connections(301), 16);
    EXPECT_EQ(adaptive_connections(51), 12);
    EXPECT_EQ(adaptive_connections(50), 8);
}

TEST(Service, WorkerErrorLoggerFormatsDiagnostics) {
    std::string logged;
    const auto logger =
        worker_error_logger([&logged](std::string_view message) { logged = message; },
                            ServiceId::speedtest, Phase::download);
    logger(0, 0, Error::make("reset"));
    EXPECT_EQ(logged, "speedtest.ru · скачивание: поток=1, попытка=1, переподключение=да: reset");
    EXPECT_FALSE(worker_error_logger(nullptr, ServiceId::yandex, Phase::upload));
}

TEST(Http, ParsesMediaTypesLikeGo) {
    EXPECT_EQ(parse_media_type("application/json"), "application/json");
    EXPECT_EQ(parse_media_type(" Application/JSON ; charset=utf-8"), "application/json");
    EXPECT_EQ(parse_media_type("text/html; charset=\"utf-8\";"), "text/html");
    EXPECT_FALSE(parse_media_type(""));
    EXPECT_FALSE(parse_media_type("application/"));
    EXPECT_FALSE(parse_media_type("application/json; charset"));
    EXPECT_FALSE(parse_media_type("text/html; a=1; A=2"));
    EXPECT_FALSE(parse_media_type("text/html extra"));
}

TEST(Http, DecodeJSONLimitedRequiresOneBoundedValue) {
    MockServer server([](MockExchange& exchange) {
        const auto& path = exchange.request().path;
        if (path == "/ok") {
            exchange.respond(200, {{"Content-Type", "application/json; charset=utf-8"}},
                             R"({"value":7})");
        } else if (path == "/two") {
            exchange.respond(200, {}, R"({"value":7}{})");
        } else if (path == "/spaces") {
            exchange.respond(200, {}, std::string(65, ' '));
        } else {
            exchange.respond(200, {}, "{");
        }
    });
    net::HttpClientOptions options;
    options.tls = testing::test_tls_context();
    const auto decode = [&](const std::string& path) {
        net::HttpSession session(options);
        net::HttpRequest request;
        request.url = *net::Url::parse(server.url(path));
        auto response = session.send(Context(), request);
        EXPECT_TRUE(response);
        EXPECT_FALSE(validate_content_type(*response, {"application/json"}) && path == "/ok");
        return decode_json_limited(Context(), *response, 64);
    };
    auto value = decode("/ok");
    ASSERT_TRUE(value) << value.error().message();
    EXPECT_EQ(value->at("value").as_int64(), 7);
    EXPECT_FALSE(decode("/two"));
    auto oversized = decode("/spaces");
    ASSERT_FALSE(oversized);
    EXPECT_EQ(oversized.error().message(), "ответ превышает безопасный предел 64 байт");
    EXPECT_FALSE(decode("/broken"));
}

} // namespace
} // namespace puls::service
