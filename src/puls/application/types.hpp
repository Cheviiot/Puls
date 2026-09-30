#pragma once

#include "puls/core/error.hpp"
#include "puls/service/service.hpp"

#include <boost/json/value.hpp>

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Coordinates measurement services independently from a concrete user
// interface. The terminal and graphical frontends use these types and the
// Runner.
namespace puls::app {

inline constexpr int schema_version = 1;

enum class Command : std::uint8_t { measure, ip };
enum class Profile : std::uint8_t { quick, balanced, accurate };
enum class PhaseSelection : std::uint8_t { all, ping, download, upload };

std::string_view to_string(Command command) noexcept;
std::string_view to_string(Profile profile) noexcept;
std::string_view to_string(PhaseSelection selection) noexcept;
std::optional<Profile> parse_profile(std::string_view text);
std::optional<PhaseSelection> parse_phase_selection(std::string_view text);
// Native phase duration of a profile: 5, 10 or 15 seconds.
std::chrono::seconds profile_duration(Profile profile) noexcept;

struct MeasureRequest {
    service::ServiceId service = service::ServiceId::yandex;
    Profile profile = Profile::balanced;
    // Zero selects the profile duration.
    std::chrono::nanoseconds duration{};
    // Zero selects the native number of connections.
    int connections = 0;
    PhaseSelection only = PhaseSelection::all;
    // Explicit speedtest.ru server; only valid with the speedtest service.
    std::string server;
    bool show_connection = false;
};

struct ConnectionRequest {
    // Empty with explicit == false uses speedtest.ru and falls back to Yandex.
    std::optional<service::ServiceId> service;
    bool explicit_service = false;
};

struct ErrorResult {
    service::ErrorCode code = service::ErrorCode::unavailable;
    service::Phase phase = service::Phase::select;
    std::string message;
    bool retryable = false;
};

struct PhaseResult {
    service::Status status = service::Status::pending;
    std::optional<double> value_ms;
    std::optional<double> min_ms;
    std::optional<double> median_ms;
    std::optional<double> average_ms;
    std::optional<double> jitter_ms;
    std::optional<int> samples;
    std::optional<std::string> method;
    std::optional<double> mbps;
    std::optional<std::int64_t> bytes;
    std::optional<double> elapsed_ms;
    std::optional<int> successful_streams;
    std::optional<int> failed_streams;
    std::vector<std::string> warnings;
    std::optional<ErrorResult> error;
};

struct ServerResult {
    std::string name;
    std::optional<std::string> city;
    std::optional<std::string> region;
};

struct MeasurementPhases {
    PhaseResult select;
    PhaseResult ping;
    PhaseResult download;
    PhaseResult upload;
};

struct MeasurementResult {
    service::ServiceId service = service::ServiceId::yandex;
    service::Status status = service::Status::pending;
    std::optional<ServerResult> server;
    MeasurementPhases phases;
    std::optional<ErrorResult> error;
    std::vector<std::string> warnings;
};

struct ConnectionResult {
    service::Status status = service::Status::pending;
    std::optional<std::string> external_ip;
    std::optional<std::string> isp;
    std::optional<service::ServiceId> detected_by;
    std::vector<std::string> warnings;
    std::optional<ErrorResult> error;
};

// JSON schema 1 envelope shared by every command.
struct Envelope {
    Command command = Command::measure;
    service::Status status = service::Status::pending;
    std::optional<ConnectionResult> connection;
    std::vector<MeasurementResult> results;
};

enum class EventKind : std::uint8_t {
    run_started,
    connection_started,
    connection_completed,
    service_started,
    phase_started,
    server_selected,
    ping_completed,
    throughput_progress,
    phase_completed,
    service_completed,
    run_completed,
};

// An immutable snapshot of a runner transition; observers may keep copies.
struct RunEvent {
    EventKind kind = EventKind::run_started;
    std::optional<service::ServiceId> service;
    std::optional<service::Phase> phase;
    std::optional<service::Server> server;
    std::optional<service::PingResult> ping;
    std::optional<service::ThroughputProgress> throughput;
    std::optional<PhaseResult> phase_result;
    std::optional<MeasurementResult> measurement;
    std::optional<ConnectionResult> connection;
    std::optional<Envelope> envelope;
};

// Observers must return promptly; a GUI should enqueue the snapshot and
// update widgets on its own thread.
using Observer = std::function<void(const RunEvent&)>;

Envelope new_envelope(Command command);
MeasurementResult new_measurement_result(service::ServiceId id, PhaseSelection selection);
PhaseResult empty_phase(service::Status status);
PhaseResult ping_phase(const service::PingResult& value);
// A failed transfer never becomes a successful zero: mbps stays null.
PhaseResult throughput_phase(const service::ThroughputResult& value, const Error& error);
ErrorResult phase_error(service::Phase phase, const Error& error);
void set_phase_error(MeasurementResult& result, PhaseResult& phase, service::Phase phase_id,
                     const Error& error);
service::Status measurement_status(const MeasurementResult& result);
service::Status aggregate_status(const std::vector<MeasurementResult>& results);
// User-facing text that keeps protocol details out of normal output.
std::string human_error(const Error& error);
void append_unique(std::vector<std::string>& destination, const std::vector<std::string>& values);
std::optional<std::string> non_empty(std::string value);
double round2(double value);

boost::json::value to_json(const Envelope& envelope);
// Pretty JSON with a trailing newline, byte-compatible with schema 1 output.
Result<std::string> encode_json(const Envelope& envelope);

} // namespace puls::app
