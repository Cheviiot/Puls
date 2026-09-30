#include "puls/core/text.hpp"
#include "puls/service/http.hpp"
#include "puls/service/speedtestru/internal.hpp"

#include <memory>

namespace puls::speedtestru {

namespace {

using namespace std::chrono_literals;
using service::ErrorCode;
using service::Phase;
using service::ServiceId;

const ErrorTag ping_only_fallback_tag{"speedtestru.errPingOnlyFallback"};

service::ThroughputOutcome failure(Phase phase, ErrorCode code, bool retryable, Error cause) {
    return {{}, service::new_error(ServiceId::speedtest, phase, code, retryable, std::move(cause))};
}

service::ThroughputOutcome finish(Phase phase, const measure::RunOutcome& outcome) {
    service::ThroughputOutcome result{service::convert_run_result(outcome.result), {}};
    if (outcome.error) {
        const ErrorCode code = service::classify_error(outcome.error);
        result.error = service::new_error(ServiceId::speedtest, phase, code,
                                          service::retryable_code(code), outcome.error);
    }
    return result;
}

} // namespace

namespace detail {
bool is_ping_only_fallback(const Error& error) {
    return error.is(ping_only_fallback_tag);
}
} // namespace detail

Result<std::vector<Backend::QmsServer>> Backend::measurement_servers() const {
    std::lock_guard lock(mutex_);
    if (servers_.empty()) {
        return Error::make("сначала необходимо выбрать сервер");
    }
    if (!throughput_) {
        return Error::join(
            {Error::tagged(ping_only_fallback_tag,
                           "резервный список speedtest.ru поддерживает только проверку задержки"),
             discovery_error_});
    }
    return servers_;
}

service::ThroughputOutcome Backend::download(const Context& ctx,
                                             const service::MeasurementConfig& config,
                                             const service::ProgressFn& progress) {
    auto servers = measurement_servers();
    if (!servers) {
        const bool ping_only = detail::is_ping_only_fallback(servers.error());
        return failure(Phase::download, ping_only ? ErrorCode::auth : ErrorCode::internal,
                       ping_only, servers.error());
    }
    const auto limits =
        service::connection_limits(config, std::min(16, static_cast<int>(servers->size()) * 2));
    if (!limits) {
        return failure(Phase::download, ErrorCode::internal, false, limits.error());
    }
    if (auto token = ensure_jwt(ctx, false, {}); !token) {
        const ErrorCode code = service::classify_error(token.error());
        return failure(Phase::download, code, service::retryable_code(code), token.error());
    }

    auto attempts =
        std::make_unique<std::atomic<std::uint32_t>[]>(static_cast<std::size_t>(limits->second));
    measure::RunConfig run;
    run.duration = config.duration;
    run.warmup = 2s;
    run.startup_timeout = 10s;
    run.ready_grace = 500ms;
    run.initial_workers = limits->first;
    run.max_workers = limits->second;
    run.reconnects = 1;
    run.adaptive = service::adaptive_connections;
    run.on_worker_error =
        service::worker_error_logger(options_.log, ServiceId::speedtest, Phase::download);
    const auto outcome = measure::run(
        ctx, run,
        [this, &servers, &attempts](const Context& worker_ctx, int index,
                                    const measure::ReadyFn& ready,
                                    const measure::RecordFn& record) -> Error {
            const QmsServer& server = (*servers)[detail::next_server_index(
                servers->size(), index, attempts[static_cast<std::size_t>(index)])];
            net::HttpSession session(measurement_options());
            std::vector<char> buffer(64 << 10);
            int chunk_mb = 25;
            while (!worker_ctx.done()) {
                const auto started = std::chrono::steady_clock::now();
                auto received =
                    download_request(worker_ctx, session, server, chunk_mb, buffer, ready, record);
                if (!received) {
                    return received.error();
                }
                chunk_mb = detail::next_download_chunk_mb(
                    *received, std::chrono::steady_clock::now() - started);
            }
            return worker_ctx.err();
        },
        service::adapt_progress(progress));
    return finish(Phase::download, outcome);
}

Result<std::int64_t> Backend::download_request(const Context& ctx, net::HttpSession& session,
                                               const QmsServer& server, int chunk_mb,
                                               std::span<char> buffer,
                                               const measure::ReadyFn& ready,
                                               const measure::RecordFn& record) {
    if (chunk_mb < 1 || chunk_mb > 250) {
        return service::protocol_error(
            "неверный размер блока скачивания: " + std::to_string(chunk_mb) + " МБ");
    }
    if (buffer.empty()) {
        return service::protocol_error("буфер скачивания не может быть пустым");
    }
    const std::uint64_t expected = static_cast<std::uint64_t>(chunk_mb) * 1'000'000;
    auto url =
        net::Url::parse(server.http_url() + "download.php?ckSize=" + std::to_string(chunk_mb) +
                        "&r=" + detail::unix_nanos_base36());
    if (!url) {
        return url.error();
    }
    for (int auth_attempt = 0; auth_attempt < 2; ++auth_attempt) {
        auto token = ensure_jwt(ctx, false, {});
        if (!token) {
            return token.error();
        }
        net::HttpRequest request;
        request.url = *url;
        request.headers = {{"jwt", *token},
                           {"User-Agent", std::string(service::user_agent)},
                           {"Accept-Encoding", "identity"}};
        auto response = session.send(ctx, request);
        if (!response) {
            return response.error();
        }
        const int status = response->status_code();
        if (detail::is_auth_status(status)) {
            service::drain_and_close(ctx, *response);
            if (auth_attempt > 0) {
                return service::authorization_error(
                    "авторизация скачивания отклонена после обновления JWT: " + response->status());
            }
            if (auto refreshed = ensure_jwt(ctx, true, *token); !refreshed) {
                return refreshed.error();
            }
            continue;
        }
        if (status < 200 || status >= 300) {
            service::drain_and_close(ctx, *response);
            return service::http_status_error(status, response->status(),
                                              "сервер " + server.host + " при скачивании");
        }
        const std::string encoding(text::trim_space(response->header("Content-Encoding")));
        if (!encoding.empty() && !text::equal_fold_ascii(encoding, "identity")) {
            service::drain_and_close(ctx, *response);
            return service::protocol_error(
                "сервер " + server.host + " применил недопустимое сжатие " + text::quote(encoding));
        }
        if (const auto length = response->content_length(); length && *length != expected) {
            service::drain_and_close(ctx, *response);
            return service::protocol_error("Content-Length при скачивании равен " +
                                           std::to_string(*length) + ", ожидалось " +
                                           std::to_string(expected));
        }
        ready();
        std::uint64_t total = 0;
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
            if (total + *count > expected) {
                response->close();
                return service::protocol_error("при скачивании получено больше ожидаемых " +
                                               std::to_string(expected) + " байт");
            }
            total += *count;
            record(static_cast<std::int64_t>(*count));
        }
        if (total != expected) {
            return service::protocol_error("при скачивании получено " + std::to_string(total) +
                                           " байт, ожидалось " + std::to_string(expected));
        }
        return static_cast<std::int64_t>(total);
    }
    return service::authorization_error(
        "авторизация скачивания не удалась после обновления токена");
}

service::ThroughputOutcome Backend::upload(const Context& ctx,
                                           const service::MeasurementConfig& config,
                                           const service::ProgressFn& progress) {
    auto servers = measurement_servers();
    if (!servers) {
        const bool ping_only = detail::is_ping_only_fallback(servers.error());
        return failure(Phase::upload, ping_only ? ErrorCode::auth : ErrorCode::internal, ping_only,
                       servers.error());
    }
    const auto limits =
        service::connection_limits(config, std::min(16, static_cast<int>(servers->size()) * 2));
    if (!limits) {
        return failure(Phase::upload, ErrorCode::internal, false, limits.error());
    }
    if (auto token = ensure_jwt(ctx, false, {}); !token) {
        const ErrorCode code = service::classify_error(token.error());
        return failure(Phase::upload, code, service::retryable_code(code), token.error());
    }

    const std::string payload(detail::upload_block_size, '\0');
    auto attempts =
        std::make_unique<std::atomic<std::uint32_t>[]>(static_cast<std::size_t>(limits->second));
    measure::RunConfig run;
    run.duration = config.duration;
    run.warmup = 4s;
    run.startup_timeout = 12s;
    run.ready_grace = 500ms;
    run.initial_workers = limits->first;
    run.max_workers = limits->second;
    run.reconnects = 1;
    run.adaptive = service::adaptive_connections;
    run.on_worker_error =
        service::worker_error_logger(options_.log, ServiceId::speedtest, Phase::upload);
    const auto outcome = measure::run(
        ctx, run,
        [this, &servers, &attempts, &payload](const Context& worker_ctx, int index,
                                              const measure::ReadyFn& ready,
                                              const measure::RecordFn& record) -> Error {
            const QmsServer& server = (*servers)[detail::next_server_index(
                servers->size(), index, attempts[static_cast<std::size_t>(index)])];
            net::HttpSession session(measurement_options());
            while (!worker_ctx.done()) {
                if (Error error = upload_request(worker_ctx, session, server, payload, ready)) {
                    return error;
                }
                record(static_cast<std::int64_t>(payload.size()));
            }
            return worker_ctx.err();
        },
        service::adapt_progress(progress));
    return finish(Phase::upload, outcome);
}

Error Backend::upload_request(const Context& ctx, net::HttpSession& session,
                              const QmsServer& server, std::string_view payload,
                              const measure::ReadyFn& ready) {
    if (payload.size() != detail::upload_block_size) {
        return service::protocol_error(
            "неверный размер блока отдачи: " + std::to_string(payload.size()) +
            " байт, ожидалось " + std::to_string(detail::upload_block_size));
    }
    auto url = net::Url::parse(server.http_url() + "upload.php?r=" + detail::unix_nanos_base36());
    if (!url) {
        return url.error();
    }
    for (int auth_attempt = 0; auth_attempt < 2; ++auth_attempt) {
        auto token = ensure_jwt(ctx, false, {});
        if (!token) {
            return token.error();
        }
        net::HttpRequest request;
        request.method = "POST";
        request.url = *url;
        request.headers = {{"jwt", *token},
                           {"User-Agent", std::string(service::user_agent)},
                           {"Content-Type", "application/octet-stream"}};
        request.body = payload;
        auto response = session.send(ctx, request);
        if (!response) {
            return response.error();
        }
        auto response_bytes = response->discard(ctx, detail::max_upload_response + 1);
        response->close();
        const int status = response->status_code();
        if (detail::is_auth_status(status)) {
            if (auth_attempt > 0) {
                return service::authorization_error(
                    "авторизация отдачи отклонена после обновления JWT: " + response->status());
            }
            if (auto refreshed = ensure_jwt(ctx, true, *token); !refreshed) {
                return refreshed.error();
            }
            continue;
        }
        if (status < 200 || status >= 300) {
            return service::http_status_error(status, response->status(),
                                              "сервер " + server.host + " при отдаче");
        }
        if (!response_bytes) {
            return response_bytes.error();
        }
        if (*response_bytes > detail::max_upload_response) {
            return service::protocol_error("ответ сервера при отдаче превышает безопасный предел");
        }
        ready();
        return {};
    }
    return service::authorization_error("авторизация отдачи не удалась после обновления токена");
}

} // namespace puls::speedtestru
