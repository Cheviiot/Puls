#include "puls/service/http.hpp"
#include "puls/service/yandex/backend.hpp"
#include "puls/service/yandex/internal.hpp"

#include <algorithm>
#include <atomic>
#include <limits>
#include <thread>

namespace puls::yandex {

namespace {

using namespace std::chrono_literals;
using service::ErrorCode;
using service::Phase;
using service::ServiceId;

constexpr int samples_per_url = 4;
// Discovery may list many CDNs; at most this many are probed at a time.
constexpr std::size_t max_parallel_cdns = 8;

struct LatencyResult {
    std::vector<double> samples;
    Error error;
};

} // namespace

Result<service::PingResult> Backend::ping(const Context& ctx) {
    std::vector<std::string> urls;
    {
        std::lock_guard lock(mutex_);
        urls = latency_urls_;
    }
    if (urls.empty()) {
        return service::new_error(ServiceId::yandex, Phase::ping, ErrorCode::internal, false,
                                  Error::make("сначала необходимо выбрать сервер"));
    }

    // Requests to one CDN are sequential over a reused connection, so the
    // first sample includes the connection setup; CDNs are measured in
    // parallel by a bounded number of threads.
    std::vector<LatencyResult> results(urls.size());
    std::atomic<std::size_t> next{0};
    const auto probe_cdns = [this, &ctx, &urls, &results, &next] {
        for (;;) {
            const std::size_t index = next.fetch_add(1);
            if (index >= urls.size() || ctx.done()) {
                return;
            }
            net::HttpSession session(http_options());
            std::vector<Error> failures;
            for (int sample = 0; sample < samples_per_url; ++sample) {
                const CancelScope probe(ctx, 5s);
                auto duration = ping_once(probe.context(), session, urls[index]);
                if (!duration) {
                    failures.push_back(duration.error());
                    session.close();
                    continue;
                }
                results[index].samples.push_back(
                    std::chrono::duration<double, std::milli>(*duration).count());
            }
            results[index].error = Error::join(std::move(failures));
        }
    };
    std::vector<std::thread> threads;
    const std::size_t thread_count = std::min(urls.size(), max_parallel_cdns);
    threads.reserve(thread_count);
    for (std::size_t worker = 0; worker < thread_count; ++worker) {
        threads.emplace_back(probe_cdns);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    if (ctx.done()) {
        return service::new_error(ServiceId::yandex, Phase::ping, ErrorCode::canceled, false,
                                  ctx.err());
    }

    std::vector<double> samples;
    const std::vector<double>* winning_cdn = nullptr;
    double winning_latency = std::numeric_limits<double>::max();
    std::vector<Error> failures;
    for (const auto& probe : results) {
        if (probe.error) {
            failures.push_back(probe.error);
        }
        samples.insert(samples.end(), probe.samples.begin(), probe.samples.end());
        for (const double value : probe.samples) {
            if (value < winning_latency) {
                winning_latency = value;
                winning_cdn = &probe.samples;
            }
        }
    }
    if (samples.empty()) {
        Error cause = Error::join(std::move(failures));
        if (!cause) {
            cause = Error::make("все запросы задержки завершились с ошибкой");
        }
        const ErrorCode code = service::classify_error(cause);
        return service::new_error(ServiceId::yandex, Phase::ping, code,
                                  service::retryable_code(code), cause);
    }
    service::PingResult result = service::stats_with_method(samples, "minimum");
    if (winning_cdn != nullptr && winning_cdn->size() > 2) {
        result.jitter_ms = service::median_absolute_deviation(
            std::vector<double>(winning_cdn->begin() + 1, winning_cdn->end()));
    } else {
        result.jitter_ms = 0;
    }
    return result;
}

Result<std::chrono::nanoseconds> Backend::ping_once(const Context& ctx, net::HttpSession& session,
                                                    const std::string& url) const {
    auto parsed = net::Url::parse(detail::cache_bust(url));
    if (!parsed) {
        return parsed.error();
    }
    net::HttpRequest request;
    request.url = std::move(parsed).value();
    request.headers = {{"User-Agent", std::string(service::user_agent)},
                       {"Accept-Encoding", "identity"}};
    const auto started = std::chrono::steady_clock::now();
    auto response = session.send(ctx, request);
    if (!response) {
        return response.error();
    }
    const auto latency = std::chrono::steady_clock::now() - started;
    const int status = response->status_code();
    if (status < 200 || status >= 300) {
        return service::http_status_error(status, response->status(), "запрос задержки");
    }
    auto body_bytes = response->discard(ctx, detail::max_ping_body_size + 1);
    if (!body_bytes) {
        return body_bytes.error();
    }
    if (*body_bytes > detail::max_ping_body_size) {
        return service::protocol_error("ответ на запрос задержки превышает безопасный предел");
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(latency);
}

} // namespace puls::yandex
