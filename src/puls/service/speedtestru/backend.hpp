#pragma once

#include "puls/net/http.hpp"
#include "puls/net/tls.hpp"
#include "puls/net/websocket.hpp"
#include "puls/service/service.hpp"

#include <boost/json/value.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The first-party QMS protocol of speedtest.ru. Server discovery and
// short-lived JWT issuance use the public browser API; measurements go
// directly to the selected QMS hosts.
namespace puls::speedtestru {

inline constexpr std::string_view default_api_base = "https://speedtest.ru";
inline constexpr std::string_view default_browser_key = "5f3287b55fbcd8076919114885f8f3f7";

struct Options {
    // Explicit QMS server (host or host:port); empty selects automatically.
    std::string server;
    service::LogFunc log;
    // Null selects the system trust store.
    std::shared_ptr<net::TlsContext> tls;
    std::string api_base = std::string(default_api_base);
    std::string browser_key = std::string(default_browser_key);
};

class Backend final : public service::Backend, public service::ConnectionInfoBackend {
public:
    explicit Backend(Options options = {});

    [[nodiscard]] service::ServiceId id() const override { return service::ServiceId::speedtest; }
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
    // Reads the public IP (required) and Internet service provider (best
    // effort) from the endpoints used by the official browser client.
    Result<service::ConnectionInfo> detect_connection(const Context& ctx) override;

    // Gives tests access to protocol internals.
    struct TestAccess;

private:
    struct QmsServer {
        int id = 0;
        std::string host;
        std::string name;
        std::string city;
        std::string region;
        std::chrono::nanoseconds rtt{};
        service::PingResult ping;

        [[nodiscard]] std::string websocket_url() const { return "wss://" + host + "/"; }
        [[nodiscard]] std::string http_url() const { return "https://" + host + "/"; }
    };

    template <class T>
    struct Fetch {
        T value{};
        int status = 0;
        Error error;
    };

    struct ExternalIp {
        std::string ip;
    };
    struct Asn {
        std::string ip;
        std::string provider_name;
    };

    [[nodiscard]] net::HttpClientOptions api_options() const;
    [[nodiscard]] net::HttpClientOptions measurement_options() const;
    [[nodiscard]] net::WebSocketOptions websocket_options() const;
    void log(std::string_view message) const;
    [[nodiscard]] std::string current_browser_key() const;

    // Discovery.
    Fetch<std::vector<QmsServer>> fetch_nearest(const Context& ctx, const std::string& key) const;
    Result<std::string> extract_browser_key(const Context& ctx) const;
    Result<std::string> refresh_browser_key(const Context& ctx, const std::string& previous);
    template <class T, class FetchFunction>
    Result<T> with_browser_key(const Context& ctx, FetchFunction fetch);
    std::vector<QmsServer> select_responsive(const Context& ctx,
                                             std::vector<QmsServer> servers) const;
    Result<QmsServer> auto_select(const Context& ctx) const;

    // Authorization.
    Result<std::string> ensure_jwt(const Context& ctx, bool refresh, const std::string& previous);
    Fetch<std::string> fetch_jwt(const Context& ctx, const std::string& key) const;

    // Latency.
    Result<service::PingResult> probe_ping(const Context& ctx, const QmsServer& server,
                                           int sample_count) const;

    // Throughput.
    Result<std::vector<QmsServer>> measurement_servers() const;
    Result<std::int64_t> download_request(const Context& ctx, net::HttpSession& session,
                                          const QmsServer& server, int chunk_mb,
                                          std::span<char> buffer, const measure::ReadyFn& ready,
                                          const measure::RecordFn& record);
    Error upload_request(const Context& ctx, net::HttpSession& session, const QmsServer& server,
                         std::string_view payload, const measure::ReadyFn& ready);

    // Connection information.
    Fetch<boost::json::value> fetch_api_json(const Context& ctx, std::string_view path,
                                             const std::string& key, std::size_t limit) const;

    Options options_;
    mutable std::mutex mutex_;
    std::vector<QmsServer> servers_;
    QmsServer selected_;
    std::string browser_key_;
    std::string jwt_;
    bool throughput_ = false;
    Error discovery_error_;
    std::mutex auth_mutex_;
    std::mutex key_mutex_;
};

// Protocol helpers, exposed for tests.
namespace detail {
struct KnownServer {
    std::string_view host;
    std::string_view city;
};
// Ping-only fallback used when discovery is unavailable.
const std::vector<KnownServer>& known_servers();
// Validates host or host:port and applies the default port.
Result<std::string> normalize_host(std::string_view host, std::string_view default_port);
// Browser-native jitter: mean consecutive difference of the middle half.
double qms_jitter(const std::vector<double>& samples, double median);
// Next download chunk that lasts about six seconds, between 25 and 250 MB.
int next_download_chunk_mb(std::int64_t bytes, std::chrono::nanoseconds elapsed);
// Rotates reconnecting workers to the next server.
std::size_t next_server_index(std::size_t servers, int worker,
                              std::atomic<std::uint32_t>& attempts);
// Finds same-document script assets like `src="/app.js"`.
std::vector<std::string> script_sources(std::string_view html, std::size_t limit);
// Finds the key in `new QMSLibrary({id:"..."})`.
std::optional<std::string> qms_library_key(std::string_view script);
// Reports whether throughput failed because only the built-in ping list is
// available.
bool is_ping_only_fallback(const Error& error);
} // namespace detail

} // namespace puls::speedtestru
