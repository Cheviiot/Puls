#include "puls/core/json.hpp"
#include "puls/core/text.hpp"
#include "puls/net/url.hpp"
#include "puls/service/http.hpp"
#include "puls/service/speedtestru/internal.hpp"

#include <algorithm>
#include <set>
#include <thread>

namespace puls::speedtestru {

namespace {

using namespace std::chrono_literals;
using service::ErrorCode;
using service::Phase;
using service::ServiceId;

bool same_origin(const net::Url& left, const net::Url& right) {
    return text::equal_fold_ascii(left.scheme, right.scheme) &&
           text::equal_fold_ascii(left.host, right.host);
}

} // namespace

Result<service::Server> Backend::select_server(const Context& ctx) {
    if (ctx.done()) {
        const Error error = ctx.err();
        return service::new_error(ServiceId::speedtest, Phase::select,
                                  service::classify_error(error), false, error);
    }
    if (!options_.server.empty()) {
        auto host = detail::normalize_host(options_.server, "20000");
        if (!host) {
            return service::new_error(ServiceId::speedtest, Phase::select, ErrorCode::internal,
                                      false, host.error());
        }
        QmsServer server;
        server.host = *host;
        server.name = *host;
        std::lock_guard lock(mutex_);
        selected_ = server;
        servers_ = {server};
        throughput_ = true;
        discovery_error_ = {};
        return service::Server{*host, {}, {}};
    }

    auto discovered = with_browser_key<std::vector<QmsServer>>(
        ctx, [this](const Context& request_ctx, const std::string& key) {
            return fetch_nearest(request_ctx, key);
        });
    Error discovery_error;
    if (discovered && !discovered->empty()) {
        std::vector<QmsServer> responsive = select_responsive(ctx, std::move(discovered).value());
        if (!responsive.empty()) {
            if (responsive.size() > 4) {
                responsive.resize(4);
            }
            const QmsServer best = responsive.front();
            {
                std::lock_guard lock(mutex_);
                selected_ = best;
                servers_ = responsive;
                throughput_ = true;
                discovery_error_ = {};
            }
            log("speedtest.ru · поиск серверов: выбран=" + best.host +
                ", доступных вариантов=" + std::to_string(responsive.size()));
            return service::Server{best.host, best.city, best.region};
        }
        discovery_error =
            Error::make("ни один найденный сервер QMS не завершил согласование WebSocket");
    } else if (!discovered) {
        discovery_error = discovered.error();
    }
    log("speedtest.ru · поиск серверов недоступен, используется встроенный список только для "
        "задержки: " +
        discovery_error.message());
    if (ctx.done()) {
        const Error error = ctx.err();
        return service::new_error(ServiceId::speedtest, Phase::select,
                                  service::classify_error(error), false, error);
    }

    auto fallback = auto_select(ctx);
    if (!fallback) {
        const Error cause = Error::join({discovery_error, fallback.error()});
        return service::new_error(ServiceId::speedtest, Phase::select, ErrorCode::unavailable, true,
                                  Error::surround("не удалось автоматически выбрать сервер (",
                                                  cause, "); укажите адрес через --server"));
    }
    const QmsServer server = std::move(fallback).value();
    {
        std::lock_guard lock(mutex_);
        selected_ = server;
        servers_ = {server};
        throughput_ = false;
        discovery_error_ = discovery_error;
    }
    return service::Server{server.host, server.city, {}};
}

Backend::Fetch<std::vector<Backend::QmsServer>>
Backend::fetch_nearest(const Context& ctx, const std::string& key) const {
    Fetch<std::vector<QmsServer>> result;
    const CancelScope request_scope(ctx, detail::api_request_timeout);
    const Context& request_ctx = request_scope.context();
    auto url =
        net::Url::parse(options_.api_base + "/api/nearest_servers?t=" + detail::unix_millis());
    if (!url) {
        result.error = url.error();
        return result;
    }
    net::HttpSession session(api_options());
    net::HttpRequest request;
    request.url = std::move(url).value();
    request.headers = {{"x-api-key", key},
                       {"User-Agent", std::string(service::user_agent)},
                       {"Accept", "application/json"}};
    auto response = session.send(request_ctx, request);
    if (!response) {
        result.error = Error::wrap("запрос nearest_servers", response.error());
        return result;
    }
    result.status = response->status_code();
    if (result.status < 200 || result.status >= 300) {
        result.error = Error::make("nearest_servers вернул состояние " + response->status());
        return result;
    }
    if (Error error = service::validate_content_type(*response, {"application/json"})) {
        result.error = service::protocol_error(Error::wrap("nearest_servers", error));
        return result;
    }
    auto value = service::decode_json_limited(request_ctx, *response, detail::max_discovery_bytes);
    if (!value) {
        result.error = Error::wrap("разбор ответа nearest_servers", value.error());
        return result;
    }

    struct Item {
        std::int64_t id = 0;
        std::string name;
        std::string city;
        std::string src;
        std::string source;
        std::int64_t port = 0;
        std::string region_name;
    };
    std::string algorithm;
    std::vector<Item> items;
    {
        auto root = json::ObjectReader::from(*value);
        Result<std::string> read_algorithm =
            root ? root->string("algorithm") : Result<std::string>(root.error());
        Result<std::vector<json::ObjectReader>> data =
            read_algorithm ? root->objects("data")
                           : Result<std::vector<json::ObjectReader>>(read_algorithm.error());
        if (!data) {
            result.error = Error::wrap("разбор ответа nearest_servers", data.error());
            return result;
        }
        algorithm = *read_algorithm;
        for (const auto& entry : *data) {
            Item item;
            auto id = entry.integer("id");
            auto name = entry.string("name");
            auto city = entry.string("city");
            auto src = entry.string("src");
            auto source = entry.string("source");
            auto port = entry.integer("port");
            auto region = entry.string("region_name");
            for (const Error* error :
                 {id ? nullptr : &id.error(), name ? nullptr : &name.error(),
                  city ? nullptr : &city.error(), src ? nullptr : &src.error(),
                  source ? nullptr : &source.error(), port ? nullptr : &port.error(),
                  region ? nullptr : &region.error()}) {
                if (error != nullptr) {
                    result.error = Error::wrap("разбор ответа nearest_servers", *error);
                    return result;
                }
            }
            items.push_back(Item{*id, *name, *city, *src, *source, *port, *region});
        }
    }
    if (text::trim_space(algorithm).empty()) {
        result.error = Error::make("nearest_servers не вернул алгоритм выбора серверов");
        return result;
    }
    std::set<std::string> seen;
    for (const Item& item : items) {
        const auto source_url = net::Url::parse(item.src);
        if (!source_url || source_url->scheme != "https" || source_url->hostname().empty() ||
            source_url->has_user || !source_url->raw_query.empty() ||
            !source_url->fragment.empty() ||
            (!source_url->path.empty() && source_url->path != "/") || item.id <= 0 ||
            text::trim_space(item.name).empty() || text::trim_space(item.city).empty() ||
            text::trim_space(item.source).empty() || item.port < 1 || item.port > 65535) {
            continue;
        }
        const std::string host =
            net::join_host_port(source_url->hostname(), std::to_string(item.port));
        if (!seen.insert(host).second) {
            continue;
        }
        QmsServer server;
        server.id = static_cast<int>(item.id);
        server.host = host;
        server.name = item.name;
        server.city = item.city;
        server.region = item.region_name;
        result.value.push_back(std::move(server));
    }
    if (result.value.empty()) {
        result.error = Error::make("nearest_servers не вернул корректных адресов");
    }
    return result;
}

Result<std::string> Backend::extract_browser_key(const Context& ctx) const {
    const CancelScope request_scope(ctx, detail::api_request_timeout);
    const Context& request_ctx = request_scope.context();
    auto base = net::Url::parse(options_.api_base);
    auto page_url = net::Url::parse(options_.api_base + "/");
    if (!base || !page_url) {
        return base ? page_url.error() : base.error();
    }
    net::HttpSession session(api_options());
    net::HttpRequest request;
    request.url = *page_url;
    request.headers = {{"User-Agent", std::string(service::user_agent)}};
    auto response = session.send(request_ctx, request);
    if (!response) {
        return Error::wrap("загрузка страницы speedtest.ru", response.error());
    }
    if (response->status_code() < 200 || response->status_code() >= 300) {
        return Error::make("страница speedtest.ru вернула состояние " + response->status());
    }
    if (Error error =
            service::validate_content_type(*response, {"text/html", "application/xhtml+xml"})) {
        return service::protocol_error(Error::wrap("страница speedtest.ru", error));
    }
    auto html = service::read_limited(request_ctx, *response, detail::max_page_bytes);
    if (!html) {
        return Error::wrap("чтение страницы speedtest.ru", html.error());
    }
    response->close();
    const std::vector<std::string> assets = detail::script_sources(*html, 32);
    if (assets.empty()) {
        return Error::make("не удалось найти адрес JS-файла страницы");
    }
    for (const std::string& asset : assets) {
        auto reference = net::Url::parse(asset);
        if (!reference) {
            continue;
        }
        const net::Url asset_url = base->resolve_reference(*reference);
        if (!same_origin(*base, asset_url)) {
            continue;
        }
        net::HttpRequest asset_request;
        asset_request.url = asset_url;
        asset_request.headers = {{"User-Agent", std::string(service::user_agent)}};
        auto asset_response = session.send(request_ctx, asset_request);
        if (!asset_response) {
            continue;
        }
        const int status = asset_response->status_code();
        auto body = service::read_limited(request_ctx, *asset_response, detail::max_page_bytes);
        const Error type_error = service::validate_content_type(
            *asset_response, {"application/javascript", "text/javascript", "text/plain"});
        const bool same_final_origin = same_origin(*base, asset_response->url());
        asset_response->close();
        if (!body || type_error || status < 200 || status >= 300 || !same_final_origin) {
            continue;
        }
        if (auto key = detail::qms_library_key(*body)) {
            return std::move(*key);
        }
    }
    return Error::make("в текущем JS-файле страницы не найден ключ браузерного клиента QMS");
}

Result<std::string> Backend::refresh_browser_key(const Context& ctx, const std::string& previous) {
    std::lock_guard key_lock(key_mutex_);
    if (const std::string current = current_browser_key();
        !current.empty() && current != previous) {
        return current;
    }
    auto refreshed = extract_browser_key(ctx);
    if (!refreshed) {
        return refreshed.error();
    }
    {
        std::lock_guard lock(mutex_);
        browser_key_ = *refreshed;
        jwt_.clear();
    }
    log("speedtest.ru · ключ браузерного клиента обновлён из текущего JS-файла сервиса");
    return refreshed;
}

std::vector<Backend::QmsServer> Backend::select_responsive(const Context& ctx,
                                                           std::vector<QmsServer> servers) const {
    if (servers.size() > 8) {
        servers.resize(8);
    }
    const CancelScope probe_scope(ctx, 8s);
    const Context& probe_ctx = probe_scope.context();
    std::mutex mutex;
    std::size_t next = 0;
    std::vector<QmsServer> responsive;
    std::vector<std::thread> workers;
    const std::size_t worker_count = std::min<std::size_t>(4, servers.size());
    for (std::size_t worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&] {
            for (;;) {
                QmsServer candidate;
                {
                    std::lock_guard lock(mutex);
                    if (next >= servers.size() || probe_ctx.done()) {
                        return;
                    }
                    candidate = servers[next++];
                }
                auto ping = probe_ping(probe_ctx, candidate, detail::ping_samples);
                if (!ping || probe_ctx.done()) {
                    continue;
                }
                candidate.ping = *ping;
                candidate.rtt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::duration<double, std::milli>(ping->median_ms));
                std::lock_guard lock(mutex);
                responsive.push_back(std::move(candidate));
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    std::stable_sort(
        responsive.begin(), responsive.end(),
        [](const QmsServer& left, const QmsServer& right) { return left.rtt < right.rtt; });
    return responsive;
}

Result<Backend::QmsServer> Backend::auto_select(const Context& ctx) const {
    const CancelScope probe_scope(ctx, 8s);
    const Context& probe_ctx = probe_scope.context();
    const auto& known = detail::known_servers();
    std::mutex mutex;
    std::size_t next = 0;
    std::optional<QmsServer> best;
    std::vector<std::thread> workers;
    for (std::size_t worker = 0; worker < std::min<std::size_t>(3, known.size()); ++worker) {
        workers.emplace_back([&] {
            for (;;) {
                QmsServer candidate;
                {
                    std::lock_guard lock(mutex);
                    if (next >= known.size() || probe_ctx.done()) {
                        return;
                    }
                    candidate.host = std::string(known[next].host);
                    candidate.name = candidate.host;
                    candidate.city = std::string(known[next].city);
                    ++next;
                }
                auto ping = probe_ping(probe_ctx, candidate, detail::ping_samples);
                if (!ping || probe_ctx.done()) {
                    continue;
                }
                candidate.ping = *ping;
                candidate.rtt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::duration<double, std::milli>(ping->median_ms));
                std::lock_guard lock(mutex);
                if (!best || candidate.rtt < best->rtt) {
                    best = std::move(candidate);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    if (!best) {
        return Error::make("ни один известный сервер не ответил вовремя");
    }
    return std::move(*best);
}

} // namespace puls::speedtestru
