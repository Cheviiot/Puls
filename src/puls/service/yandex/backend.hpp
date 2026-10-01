#pragma once

#include "puls/net/http.hpp"
#include "puls/net/tls.hpp"
#include "puls/service/service.hpp"

#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The public protocol of Яндекс.Интернетометр (https://yandex.ru/internet/).
namespace puls::yandex {

inline constexpr std::string_view default_probes_url =
    "https://yandex.ru/internet/api/v0/get-probes?flag_ws-conn-timeout=2000";
inline constexpr std::string_view default_internet_page_url = "https://yandex.ru/internet/";

struct Options {
    service::LogFunc log;
    // Null selects the system trust store.
    std::shared_ptr<net::TlsContext> tls;
    std::string probes_url = std::string(default_probes_url);
    std::string internet_page_url = std::string(default_internet_page_url);
};

class Backend final : public service::Backend, public service::ConnectionInfoBackend {
public:
    explicit Backend(Options options = {});

    [[nodiscard]] service::ServiceId id() const override { return service::ServiceId::yandex; }
    [[nodiscard]] service::Capability capabilities() const override {
        return service::Capability::ping | service::Capability::download |
               service::Capability::upload;
    }
    Result<service::Server> select_server(const Context& ctx) override;
    Result<service::PingResult> ping(const Context& ctx) override;
    service::ThroughputOutcome download(const Context& ctx,
                                        const service::MeasurementConfig& config,
                                        const service::ProgressFn& progress) override;
    service::ThroughputOutcome upload(const Context& ctx, const service::MeasurementConfig& config,
                                      const service::ProgressFn& progress) override;
    // Reports the public IP address that the official page embeds in its
    // bootstrap state. Independent of select_server.
    Result<service::ConnectionInfo> detect_connection(const Context& ctx) override;

    // Gives tests access to protocol internals.
    struct TestAccess;

private:
    struct UploadProbe {
        std::string post_url;
        std::string websocket_url;
        std::chrono::nanoseconds websocket_connection_timeout{};
    };

    [[nodiscard]] net::HttpClientOptions http_options() const;
    Result<std::chrono::nanoseconds> ping_once(const Context& ctx, net::HttpSession& session,
                                               const std::string& url) const;
    Result<std::int64_t> download_probe(const Context& ctx, net::HttpSession& session,
                                        const std::string& url, std::span<char> buffer,
                                        const measure::ReadyFn& ready,
                                        const measure::RecordFn& record) const;
    Error upload_worker(const Context& ctx, net::HttpSession& session, const UploadProbe& probe,
                        std::string_view payload, const service::MeasurementConfig& config,
                        const measure::ReadyFn& ready, const measure::RecordFn& record) const;
    Error websocket_upload(const Context& ctx, const std::string& url,
                           std::chrono::nanoseconds connection_timeout,
                           std::chrono::nanoseconds duration, const measure::ReadyFn& ready,
                           const measure::RecordFn& record) const;
    Error http_upload(const Context& ctx, net::HttpSession& session, const std::string& post_url,
                      std::string_view payload, const measure::ReadyFn& ready,
                      const measure::RecordFn& record) const;
    void log(std::string_view message) const;

    Options options_;
    mutable std::mutex mutex_;
    std::vector<std::string> latency_urls_;
    std::vector<std::string> download_urls_;
    std::vector<UploadProbe> upload_probes_;
};

// Protocol helpers, exposed for tests.
namespace detail {
// Appends a unique cache-busting query parameter.
std::string cache_bust(std::string_view url);
bool valid_http_probe_url(std::string_view url);
bool valid_websocket_probe_url(std::string_view url);
// Lower-case host:port of a URL.
std::string endpoint_host(std::string_view url);
bool is_large_download_probe(std::string_view url);
std::string host_of(std::string_view url);
// Returns the first balanced "{...}" object literal that follows marker,
// respecting quoted strings.
Result<std::string_view> extract_balanced_json_object(std::string_view body,
                                                      std::string_view marker);
} // namespace detail

} // namespace puls::yandex
