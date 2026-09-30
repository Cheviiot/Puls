#include "puls/core/json.hpp"
#include "puls/core/text.hpp"
#include "puls/service/http.hpp"
#include "puls/service/yandex/backend.hpp"
#include "puls/service/yandex/internal.hpp"

#include <algorithm>
#include <set>

namespace puls::yandex {

namespace {

using service::ErrorCode;
using service::Phase;
using service::ServiceId;

struct DownloadProbe {
    std::string url;
    std::int64_t timeout = 0;
};

struct UploadProbeDocument {
    std::int64_t size = 0;
    std::string url;
    std::string post_url;
    std::string stats_url;
    std::string websocket_url;
    std::int64_t websocket_connection_timeout = 0;
    std::int64_t timeout = 0;
};

struct ProbesDocument {
    std::string mid;
    std::vector<std::string> latency;
    std::vector<DownloadProbe> download;
    std::vector<UploadProbeDocument> upload;
};

// Decodes get-probes with encoding/json struct semantics.
Result<ProbesDocument> decode_probes(const boost::json::value& value) {
    ProbesDocument document;
    auto root = json::ObjectReader::from(value);
    if (!root) {
        return std::move(root).error();
    }
    auto mid = root->string("mid");
    if (!mid) {
        return std::move(mid).error();
    }
    document.mid = std::move(mid).value();

    auto latency = root->object("latency");
    if (!latency) {
        return std::move(latency).error();
    }
    auto latency_probes = latency->objects("probes");
    if (!latency_probes) {
        return std::move(latency_probes).error();
    }
    for (const auto& probe : *latency_probes) {
        auto url = probe.string("url");
        if (!url) {
            return std::move(url).error();
        }
        document.latency.push_back(std::move(url).value());
    }

    auto download = root->object("download");
    if (!download) {
        return std::move(download).error();
    }
    auto download_probes = download->objects("probes");
    if (!download_probes) {
        return std::move(download_probes).error();
    }
    for (const auto& probe : *download_probes) {
        auto url = probe.string("url");
        auto timeout = probe.integer("timeout");
        auto start_delay = probe.integer("startDelay");
        for (const Error* error :
             {url ? nullptr : &url.error(), timeout ? nullptr : &timeout.error(),
              start_delay ? nullptr : &start_delay.error()}) {
            if (error != nullptr) {
                return *error;
            }
        }
        document.download.push_back(DownloadProbe{std::move(url).value(), *timeout});
    }

    auto upload = root->object("upload");
    if (!upload) {
        return std::move(upload).error();
    }
    auto upload_probes = upload->objects("probes");
    if (!upload_probes) {
        return std::move(upload_probes).error();
    }
    for (const auto& probe : *upload_probes) {
        auto size = probe.integer("size");
        auto url = probe.string("url");
        auto post_url = probe.string("postUrl");
        auto stats_url = probe.string("statsUrl");
        auto websocket_url = probe.string("websocketUrl");
        auto websocket_timeout = probe.integer("websocketConnectionTimeout");
        auto timeout = probe.integer("timeout");
        for (const Error* error :
             {size ? nullptr : &size.error(), url ? nullptr : &url.error(),
              post_url ? nullptr : &post_url.error(), stats_url ? nullptr : &stats_url.error(),
              websocket_url ? nullptr : &websocket_url.error(),
              websocket_timeout ? nullptr : &websocket_timeout.error(),
              timeout ? nullptr : &timeout.error()}) {
            if (error != nullptr) {
                return *error;
            }
        }
        document.upload.push_back(
            UploadProbeDocument{*size, std::move(url).value(), std::move(post_url).value(),
                                std::move(stats_url).value(), std::move(websocket_url).value(),
                                *websocket_timeout, *timeout});
    }
    return document;
}

Error schema_error(std::string message) {
    return service::new_error(ServiceId::yandex, Phase::select, ErrorCode::protocol, true,
                              Error::make(std::move(message)));
}

} // namespace

Backend::Backend(Options options) : options_(std::move(options)) {}

net::HttpClientOptions Backend::http_options() const {
    net::HttpClientOptions options;
    options.tls = options_.tls;
    options.tls_handshake_timeout = std::chrono::seconds(10);
    options.response_header_timeout = std::chrono::seconds(10);
    options.follow_redirects = false;
    return options;
}

void Backend::log(std::string_view message) const {
    if (options_.log) {
        options_.log(message);
    }
}

Result<service::Server> Backend::select_server(const Context& ctx) {
    auto url = net::Url::parse(detail::cache_bust(options_.probes_url));
    if (!url) {
        return service::new_error(ServiceId::yandex, Phase::select, ErrorCode::internal, false,
                                  url.error());
    }
    net::HttpSession session(http_options());
    net::HttpRequest request;
    request.url = std::move(url).value();
    request.headers = {{"User-Agent", std::string(service::user_agent)},
                       {"Accept", "application/json"}};
    auto response = session.send(ctx, request);
    if (!response) {
        return service::new_error(ServiceId::yandex, Phase::select, ErrorCode::unavailable, true,
                                  Error::wrap("запрос get-probes", response.error()));
    }
    const int status = response->status_code();
    if (status < 200 || status >= 300) {
        const bool retryable = status == 408 || status == 429 || status >= 500;
        const ErrorCode code = status >= 500 ? ErrorCode::unavailable : ErrorCode::protocol;
        return service::new_error(ServiceId::yandex, Phase::select, code, retryable,
                                  Error::make("get-probes вернул состояние " + response->status()));
    }
    if (Error error = service::validate_content_type(*response, {"application/json"})) {
        return service::new_error(ServiceId::yandex, Phase::select, ErrorCode::protocol, true,
                                  Error::wrap("get-probes", error));
    }
    auto value = service::decode_json_limited(ctx, *response, detail::max_discovery_body_size);
    Result<ProbesDocument> decoded =
        value ? decode_probes(*value) : Result<ProbesDocument>(value.error());
    if (!decoded) {
        return service::new_error(ServiceId::yandex, Phase::select, ErrorCode::protocol, true,
                                  Error::wrap("разбор ответа get-probes", decoded.error()));
    }
    const ProbesDocument& document = *decoded;
    if (text::trim_space(document.mid).empty()) {
        return service::new_error(ServiceId::yandex, Phase::select, ErrorCode::protocol, true,
                                  Error::make("get-probes вернул пустой идентификатор mid"));
    }

    std::vector<std::string> latency_urls;
    std::vector<std::string> latency_hosts;
    for (std::size_t index = 0; index < document.latency.size(); ++index) {
        const std::string& probe = document.latency[index];
        const std::string number = std::to_string(index + 1);
        if (!detail::valid_http_probe_url(probe)) {
            return schema_error("неверный URL latency-пробы " + number);
        }
        const std::string host = detail::endpoint_host(probe);
        if (std::find(latency_hosts.begin(), latency_hosts.end(), host) != latency_hosts.end()) {
            return schema_error("повторяющийся CDN latency-пробы " + number);
        }
        latency_hosts.push_back(host);
        latency_urls.push_back(probe);
    }

    std::vector<std::string> download_urls;
    std::set<std::string> download_hosts;
    for (std::size_t index = 0; index < document.download.size(); ++index) {
        const DownloadProbe& probe = document.download[index];
        const std::string number = std::to_string(index + 1);
        if (!detail::valid_http_probe_url(probe.url)) {
            return schema_error("неверный URL download-пробы " + number);
        }
        if (probe.timeout != 0) {
            continue;
        }
        if (!detail::is_large_download_probe(probe.url)) {
            return schema_error("неожиданный URL основной download-пробы " + number);
        }
        if (!download_hosts.insert(detail::endpoint_host(probe.url)).second) {
            return schema_error("повторяющийся CDN download-пробы " + number);
        }
        download_urls.push_back(probe.url);
    }

    std::vector<UploadProbe> upload_probes;
    std::set<std::string> upload_hosts;
    for (std::size_t index = 0; index < document.upload.size(); ++index) {
        const UploadProbeDocument& probe = document.upload[index];
        const std::string number = std::to_string(index + 1);
        if (probe.size <= 0 || !detail::valid_http_probe_url(probe.url) ||
            !detail::valid_http_probe_url(probe.post_url) ||
            !detail::valid_http_probe_url(probe.stats_url)) {
            return schema_error("неверная upload-проба " + number);
        }
        const std::string probe_host = detail::endpoint_host(probe.url);
        if (detail::endpoint_host(probe.post_url) != probe_host ||
            detail::endpoint_host(probe.stats_url) != probe_host) {
            return schema_error("endpoint upload-пробы " + number + " относятся к разным CDN");
        }
        if (!probe.websocket_url.empty()) {
            if (!detail::valid_websocket_probe_url(probe.websocket_url) ||
                detail::endpoint_host(probe.websocket_url) != probe_host) {
                return schema_error("неверный WebSocket URL upload-пробы " + number);
            }
            if (probe.websocket_connection_timeout <= 0 ||
                probe.websocket_connection_timeout > 30'000) {
                return schema_error("неверный WebSocket timeout upload-пробы " + number);
            }
        }
        if (probe.timeout != 0) {
            continue;
        }
        if (!upload_hosts.insert(probe_host).second) {
            return schema_error("повторяющийся CDN upload-пробы " + number);
        }
        upload_probes.push_back(
            UploadProbe{probe.post_url, probe.websocket_url,
                        std::chrono::milliseconds(probe.websocket_connection_timeout)});
    }

    if (latency_urls.empty() || download_urls.empty() || upload_probes.empty()) {
        return service::new_error(
            ServiceId::yandex, Phase::select, ErrorCode::protocol, true,
            Error::make("неполный набор проб (задержка=" + std::to_string(latency_urls.size()) +
                        ", скачивание=" + std::to_string(download_urls.size()) +
                        ", отдача=" + std::to_string(upload_probes.size()) + ")"));
    }
    for (const auto& host : latency_hosts) {
        if (download_hosts.count(host) == 0) {
            return schema_error("CDN " + host + " не содержит основной download-пробы");
        }
        if (upload_hosts.count(host) == 0) {
            return schema_error("CDN " + host + " не содержит основной upload-пробы");
        }
    }
    if (download_hosts.size() != latency_hosts.size() ||
        upload_hosts.size() != latency_hosts.size()) {
        return schema_error("наборы latency, download и upload относятся к разным CDN");
    }

    const std::string server_name = detail::host_of(download_urls.front());
    {
        std::lock_guard lock(mutex_);
        latency_urls_ = std::move(latency_urls);
        download_urls_ = std::move(download_urls);
        upload_probes_ = std::move(upload_probes);
    }
    return service::Server{server_name, {}, {}};
}

} // namespace puls::yandex
