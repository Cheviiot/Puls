#include "puls/core/text.hpp"
#include "puls/service/http.hpp"
#include "puls/service/yandex/backend.hpp"
#include "puls/service/yandex/internal.hpp"

namespace puls::yandex {

namespace {

using namespace std::chrono_literals;
using service::ErrorCode;
using service::Phase;
using service::ServiceId;

service::ThroughputOutcome failure(Phase phase, ErrorCode code, bool retryable, Error cause) {
    return {{}, service::new_error(ServiceId::yandex, phase, code, retryable, std::move(cause))};
}

service::ThroughputOutcome finish(Phase phase, const measure::RunOutcome& outcome) {
    service::ThroughputOutcome result{service::convert_run_result(outcome.result), {}};
    if (outcome.error) {
        const ErrorCode code = service::classify_error(outcome.error);
        result.error = service::new_error(ServiceId::yandex, phase, code,
                                          code != ErrorCode::canceled, outcome.error);
    }
    return result;
}

} // namespace

service::ThroughputOutcome Backend::download(const Context& ctx,
                                             const service::MeasurementConfig& config,
                                             const service::ProgressFn& progress) {
    std::vector<std::string> urls;
    {
        std::lock_guard lock(mutex_);
        urls = download_urls_;
    }
    if (urls.empty()) {
        return failure(Phase::download, ErrorCode::internal, false,
                       Error::make("сначала необходимо выбрать сервер"));
    }
    const auto limits = service::connection_limits(config, static_cast<int>(urls.size()));
    if (!limits) {
        return failure(Phase::download, ErrorCode::internal, false, limits.error());
    }

    measure::RunConfig run;
    run.duration = config.duration;
    run.warmup = 750ms;
    run.startup_timeout = 8s;
    run.ready_grace = 400ms;
    run.initial_workers = limits->first;
    run.max_workers = limits->second;
    run.reconnects = 1;
    run.adaptive = service::adaptive_connections;
    run.on_worker_error =
        service::worker_error_logger(options_.log, ServiceId::yandex, Phase::download);
    const auto outcome = measure::run(
        ctx, run,
        [this, &urls](const Context& worker_ctx, int index, const measure::ReadyFn& ready,
                      const measure::RecordFn& record) -> Error {
            const std::string& url = urls[static_cast<std::size_t>(index) % urls.size()];
            net::HttpSession session(http_options());
            std::vector<char> buffer(64 << 10);
            while (!worker_ctx.done()) {
                auto received = download_probe(worker_ctx, session, url, buffer, ready, record);
                if (!received) {
                    return received.error();
                }
            }
            return worker_ctx.err();
        },
        service::adapt_progress(progress));
    return finish(Phase::download, outcome);
}

service::ThroughputOutcome Backend::upload(const Context& ctx,
                                           const service::MeasurementConfig& config,
                                           const service::ProgressFn& progress) {
    std::vector<UploadProbe> probes;
    {
        std::lock_guard lock(mutex_);
        probes = upload_probes_;
    }
    if (probes.empty()) {
        return failure(Phase::upload, ErrorCode::internal, false,
                       Error::make("сначала необходимо выбрать сервер"));
    }
    const auto limits = service::connection_limits(config, static_cast<int>(probes.size()));
    if (!limits) {
        return failure(Phase::upload, ErrorCode::internal, false, limits.error());
    }

    const std::string payload(detail::upload_chunk_size, '\0');
    measure::RunConfig run;
    run.duration = config.duration;
    run.warmup = 750ms;
    run.startup_timeout = 10s;
    run.ready_grace = 400ms;
    run.initial_workers = limits->first;
    run.max_workers = limits->second;
    run.reconnects = 1;
    run.adaptive = service::adaptive_connections;
    run.on_worker_error =
        service::worker_error_logger(options_.log, ServiceId::yandex, Phase::upload);
    const auto outcome = measure::run(
        ctx, run,
        [this, &probes, &payload, &config](const Context& worker_ctx, int index,
                                           const measure::ReadyFn& ready,
                                           const measure::RecordFn& record) {
            const UploadProbe& probe = probes[static_cast<std::size_t>(index) % probes.size()];
            net::HttpSession session(http_options());
            return upload_worker(worker_ctx, session, probe, payload, config, ready, record);
        },
        service::adapt_progress(progress));
    return finish(Phase::upload, outcome);
}

Result<std::int64_t> Backend::download_probe(const Context& ctx, net::HttpSession& session,
                                             const std::string& url, std::span<char> buffer,
                                             const measure::ReadyFn& ready,
                                             const measure::RecordFn& record) const {
    auto parsed = net::Url::parse(detail::cache_bust(url));
    if (!parsed) {
        return parsed.error();
    }
    net::HttpRequest request;
    request.url = std::move(parsed).value();
    request.headers = {{"User-Agent", std::string(service::user_agent)},
                       {"Accept-Encoding", "identity"}};
    auto response = session.send(ctx, request);
    if (!response) {
        return response.error();
    }
    const int status = response->status_code();
    if (status < 200 || status >= 300) {
        return service::http_status_error(status, response->status(), "проба скачивания");
    }
    const auto length = response->content_length();
    if (!length || *length == 0) {
        return service::protocol_error("проба скачивания не содержит положительный Content-Length");
    }
    const std::uint64_t expected = *length;
    if (expected > detail::max_download_body_size) {
        return service::protocol_error("Content-Length пробы скачивания (" +
                                       std::to_string(expected) + ") превышает безопасный предел");
    }
    if (detail::is_large_download_probe(url) && expected != detail::expected_download_size) {
        return service::protocol_error(
            "неожиданный размер пробы скачивания: " + std::to_string(expected) + " байт вместо " +
            std::to_string(detail::expected_download_size));
    }
    const std::string encoding(text::trim_space(response->header("Content-Encoding")));
    if (!encoding.empty() && !text::equal_fold_ascii(encoding, "identity")) {
        return service::protocol_error("проба скачивания использует неожиданное Content-Encoding " +
                                       text::quote(encoding));
    }
    if (Error error = service::validate_content_type(*response, {"application/octet-stream"})) {
        return service::protocol_error(Error::wrap("проба скачивания", error));
    }
    ready();
    std::uint64_t received = 0;
    for (;;) {
        auto count = response->read(ctx, buffer);
        if (!count) {
            if (ctx.done()) {
                return ctx.err();
            }
            return count.error();
        }
        if (*count == 0) {
            break;
        }
        if (received + *count > expected) {
            return service::protocol_error("проба скачивания превысила ожидаемый размер " +
                                           std::to_string(expected) + " байт");
        }
        received += *count;
        record(static_cast<std::int64_t>(*count));
    }
    if (received == 0) {
        return service::protocol_error("проба скачивания вернула пустой ответ");
    }
    if (received != expected) {
        return service::protocol_error("проба скачивания вернула " + std::to_string(received) +
                                       " байт, ожидалось " + std::to_string(expected));
    }
    return static_cast<std::int64_t>(received);
}

Error Backend::upload_worker(const Context& ctx, net::HttpSession& session,
                             const UploadProbe& probe, std::string_view payload,
                             const service::MeasurementConfig& config,
                             const measure::ReadyFn& ready, const measure::RecordFn& record) const {
    if (!probe.websocket_url.empty()) {
        Error websocket_error =
            websocket_upload(ctx, probe.websocket_url, probe.websocket_connection_timeout,
                             config.duration, ready, record);
        if (!websocket_error || ctx.done()) {
            return websocket_error;
        }
        log("Яндекс · отдача: WebSocket недоступен, переход на резервный HTTP-запрос: " +
            websocket_error.message());
    }
    return http_upload(ctx, session, probe.post_url, payload, ready, record);
}

} // namespace puls::yandex
