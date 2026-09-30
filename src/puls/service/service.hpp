#pragma once

#include "puls/core/context.hpp"
#include "puls/core/error.hpp"
#include "puls/core/ip.hpp"
#include "puls/measure/measure.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Contracts shared by Internet measurement services such as Yandex
// Internetometer and speedtest.ru.
namespace puls::service {

// Identifies a measurement service in commands, results and errors.
enum class ServiceId : std::uint8_t { yandex, speedtest, all };

// An operation performed by a measurement service.
enum class Phase : std::uint8_t { select, connection, ping, download, upload };

// Stable state used by human and machine-readable results.
enum class Status : std::uint8_t { pending, skipped, ok, partial, error, canceled };

// Stable error codes for scripts; the wrapped cause stays available.
enum class ErrorCode : std::uint8_t { unavailable, timeout, protocol, auth, canceled, internal };

std::string_view to_string(ServiceId id) noexcept;
std::string_view to_string(Phase phase) noexcept;
std::string_view to_string(Status status) noexcept;
std::string_view to_string(ErrorCode code) noexcept;
std::optional<ServiceId> parse_service_id(std::string_view text);

// User-facing name of a measurement service.
std::string display_name(ServiceId id);
std::string phase_display_name(Phase phase);

// Receives optional diagnostics. Callers decide where they are rendered;
// services never write to a terminal directly. It may be called from worker
// threads concurrently.
using LogFunc = std::function<void(std::string_view message)>;

// What a Backend can measure.
enum class Capability : unsigned { none = 0, ping = 1, download = 2, upload = 4 };
constexpr Capability operator|(Capability left, Capability right) noexcept {
    return static_cast<Capability>(static_cast<unsigned>(left) | static_cast<unsigned>(right));
}
constexpr bool has(Capability set, Capability value) noexcept {
    return (static_cast<unsigned>(set) & static_cast<unsigned>(value)) != 0;
}

struct PingResult {
    // Service-native primary latency: some services report the minimum RTT,
    // others the median.
    double value_ms = 0;
    double min_ms = 0;
    double median_ms = 0;
    double avg_ms = 0;
    double jitter_ms = 0;
    int samples = 0;
    std::string method;

    friend bool operator==(const PingResult&, const PingResult&) = default;
};

using Duration = std::chrono::nanoseconds;

// Controls a throughput phase. connections == 0 asks the service for its
// native default; max_connections is a hard safety cap.
struct MeasurementConfig {
    Duration duration{};
    int connections = 0;
    int max_connections = 0;
};

// Emitted periodically while a transfer is running.
struct ThroughputProgress {
    double mbps = 0;
    std::int64_t bytes = 0;
    Duration elapsed{};
    int active_connections = 0;
};
using ProgressFn = std::function<void(const ThroughputProgress&)>;

// Only bytes confirmed by a successful HTTP or service-protocol response.
struct ThroughputResult {
    double mbps = 0;
    std::int64_t bytes = 0;
    Duration elapsed{};
    int successful_connections = 0;
    int failed_connections = 0;
    std::vector<std::string> warnings;
};

// A throughput phase returns partial diagnostics together with an error.
struct ThroughputOutcome {
    ThroughputResult result;
    Error error;
};

// The public network identity reported by a service. The IP is required for
// a successful lookup; the Internet service provider is optional.
struct ConnectionInfo {
    IpAddress external_ip;
    std::string isp;
    std::vector<std::string> warnings;
};

// The selected measurement server.
struct Server {
    std::string name;
    std::string city;
    std::string region;
};

// Implements one measurement service. Methods may block and must return
// promptly once ctx is done.
class Backend {
public:
    virtual ~Backend() = default;

    [[nodiscard]] virtual ServiceId id() const = 0;
    [[nodiscard]] virtual Capability capabilities() const = 0;
    // Picks the measurement server used by the following calls.
    virtual Result<Server> select_server(const Context& ctx) = 0;
    virtual Result<PingResult> ping(const Context& ctx) = 0;
    virtual ThroughputOutcome download(const Context& ctx, const MeasurementConfig& config,
                                       const ProgressFn& progress) = 0;
    virtual ThroughputOutcome upload(const Context& ctx, const MeasurementConfig& config,
                                     const ProgressFn& progress) = 0;
};

// Implemented by services that can report the caller's public IP address and,
// when available, Internet service provider.
class ConnectionInfoBackend {
public:
    virtual ~ConnectionInfoBackend() = default;
    virtual Result<ConnectionInfo> detect_connection(const Context& ctx) = 0;
};

// Describes which service phase failed and whether retrying can help.
class OpError final : public ErrorDetail {
public:
    OpError(ServiceId failed_service, Phase failed_phase, ErrorCode error_code,
            bool can_retry) noexcept
        : service(failed_service), phase(failed_phase), code(error_code), retryable(can_retry) {}

    ServiceId service;
    Phase phase;
    ErrorCode code;
    bool retryable;
};

// Preserves an unexpected HTTP status for typed classification.
class HttpStatusError final : public ErrorDetail {
public:
    HttpStatusError(int code, std::string status_text, std::string operation_name)
        : status_code(code), status(std::move(status_text)), operation(std::move(operation_name)) {}

    int status_code;
    std::string status;
    std::string operation;
};

// Builds an OpError. Retrying an explicit cancellation is never useful; every
// other retry decision is kept as supplied by the service.
Error new_error(ServiceId service, Phase phase, ErrorCode code, bool retryable, Error cause);
Error http_status_error(int status_code, std::string status, std::string operation);

extern const ErrorTag protocol_tag;
extern const ErrorTag authorization_tag;
// Marks validation failures in first-party service protocols.
Error protocol_error(Error cause = {});
Error protocol_error(std::string message);
// Marks rejected or invalid service authorization.
Error authorization_error(Error cause = {});
Error authorization_error(std::string message);

// Maps typed errors to stable API codes.
ErrorCode classify_error(const Error& error);
// Maps an unexpected HTTP status to a stable code and retry decision.
std::pair<ErrorCode, bool> classify_http_status(int status_code);
bool retryable_code(ErrorCode code);

// Validates a throughput configuration against the native worker count and
// returns the initial and maximum number of workers.
Result<std::pair<int, int>> connection_limits(const MeasurementConfig& config, int native);
int adaptive_connections(double mbps);
measure::ProgressFn adapt_progress(const ProgressFn& progress);
ThroughputResult convert_run_result(const measure::RunResult& result);
std::function<void(int, int, const Error&)> worker_error_logger(const LogFunc& log,
                                                                ServiceId service, Phase phase);

// Reduces round-trip-time samples (milliseconds) to min/median/average and
// mean absolute consecutive difference as jitter. Invalid samples (negative,
// infinite or NaN) are ignored.
PingResult stats(const std::vector<double>& samples_ms);
// Same statistics with the service-native primary value: "minimum", "median"
// or "average".
PingResult stats_with_method(const std::vector<double>& samples_ms, std::string_view method);
// Robust latency variation that one queued response cannot dominate.
double median_absolute_deviation(const std::vector<double>& samples_ms);

} // namespace puls::service
