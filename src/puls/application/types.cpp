#include "puls/application/types.hpp"

#include "puls/core/json.hpp"
#include "puls/core/text.hpp"

#include <algorithm>
#include <cmath>

namespace puls::app {

namespace {

using service::ErrorCode;
using service::Status;

boost::json::value optional_number(const std::optional<double>& value) {
    return value ? boost::json::value(*value) : boost::json::value(nullptr);
}

template <class Integer>
boost::json::value optional_integer(const std::optional<Integer>& value) {
    return value ? boost::json::value(static_cast<std::int64_t>(*value))
                 : boost::json::value(nullptr);
}

boost::json::value optional_string(const std::optional<std::string>& value) {
    return value ? boost::json::value(*value) : boost::json::value(nullptr);
}

boost::json::value strings(const std::vector<std::string>& values) {
    boost::json::array array;
    for (const auto& value : values) {
        array.emplace_back(value);
    }
    return array;
}

boost::json::value error_json(const std::optional<ErrorResult>& error) {
    if (!error) {
        return nullptr;
    }
    boost::json::object object;
    object["code"] = service::to_string(error->code);
    object["phase"] = service::to_string(error->phase);
    object["message"] = error->message;
    object["retryable"] = error->retryable;
    return object;
}

boost::json::value phase_json(const PhaseResult& phase) {
    boost::json::object object;
    object["status"] = service::to_string(phase.status);
    object["value_ms"] = optional_number(phase.value_ms);
    object["min_ms"] = optional_number(phase.min_ms);
    object["median_ms"] = optional_number(phase.median_ms);
    object["average_ms"] = optional_number(phase.average_ms);
    object["jitter_ms"] = optional_number(phase.jitter_ms);
    object["samples"] = optional_integer(phase.samples);
    object["method"] = optional_string(phase.method);
    object["mbps"] = optional_number(phase.mbps);
    object["bytes"] = optional_integer(phase.bytes);
    object["elapsed_ms"] = optional_number(phase.elapsed_ms);
    object["successful_streams"] = optional_integer(phase.successful_streams);
    object["failed_streams"] = optional_integer(phase.failed_streams);
    object["warnings"] = strings(phase.warnings);
    object["error"] = error_json(phase.error);
    return object;
}

boost::json::value measurement_json(const MeasurementResult& result) {
    boost::json::object object;
    object["service"] = service::to_string(result.service);
    object["status"] = service::to_string(result.status);
    if (result.server) {
        boost::json::object server;
        server["name"] = result.server->name;
        server["city"] = optional_string(result.server->city);
        server["region"] = optional_string(result.server->region);
        object["server"] = std::move(server);
    } else {
        object["server"] = nullptr;
    }
    boost::json::object phases;
    phases["select"] = phase_json(result.phases.select);
    phases["ping"] = phase_json(result.phases.ping);
    phases["download"] = phase_json(result.phases.download);
    phases["upload"] = phase_json(result.phases.upload);
    object["phases"] = std::move(phases);
    object["error"] = error_json(result.error);
    object["warnings"] = strings(result.warnings);
    return object;
}

boost::json::value connection_json(const ConnectionResult& connection) {
    boost::json::object object;
    object["status"] = service::to_string(connection.status);
    object["external_ip"] = optional_string(connection.external_ip);
    object["isp"] = optional_string(connection.isp);
    object["detected_by"] = connection.detected_by
                                ? boost::json::value(service::to_string(*connection.detected_by))
                                : boost::json::value(nullptr);
    object["warnings"] = strings(connection.warnings);
    object["error"] = error_json(connection.error);
    return object;
}

} // namespace

std::string_view to_string(Command command) noexcept {
    return command == Command::ip ? "ip" : "measure";
}

std::string_view to_string(Profile profile) noexcept {
    switch (profile) {
    case Profile::quick:
        return "quick";
    case Profile::accurate:
        return "accurate";
    case Profile::balanced:
        break;
    }
    return "balanced";
}

std::string_view to_string(PhaseSelection selection) noexcept {
    switch (selection) {
    case PhaseSelection::ping:
        return "ping";
    case PhaseSelection::download:
        return "download";
    case PhaseSelection::upload:
        return "upload";
    case PhaseSelection::all:
        break;
    }
    return "all";
}

std::optional<Profile> parse_profile(std::string_view text) {
    if (text == "quick") {
        return Profile::quick;
    }
    if (text == "balanced") {
        return Profile::balanced;
    }
    if (text == "accurate") {
        return Profile::accurate;
    }
    return std::nullopt;
}

std::optional<PhaseSelection> parse_phase_selection(std::string_view text) {
    if (text == "all") {
        return PhaseSelection::all;
    }
    if (text == "ping") {
        return PhaseSelection::ping;
    }
    if (text == "download") {
        return PhaseSelection::download;
    }
    if (text == "upload") {
        return PhaseSelection::upload;
    }
    return std::nullopt;
}

std::chrono::seconds profile_duration(Profile profile) noexcept {
    switch (profile) {
    case Profile::quick:
        return std::chrono::seconds(5);
    case Profile::accurate:
        return std::chrono::seconds(15);
    case Profile::balanced:
        break;
    }
    return std::chrono::seconds(10);
}

Envelope new_envelope(Command command) {
    Envelope envelope;
    envelope.command = command;
    envelope.status = Status::pending;
    return envelope;
}

MeasurementResult new_measurement_result(service::ServiceId id, PhaseSelection selection) {
    MeasurementResult result;
    result.service = id;
    result.status = Status::pending;
    result.phases.select = empty_phase(Status::pending);
    result.phases.ping = empty_phase(Status::skipped);
    result.phases.download = empty_phase(Status::skipped);
    result.phases.upload = empty_phase(Status::skipped);
    switch (selection) {
    case PhaseSelection::all:
        result.phases.ping.status = Status::pending;
        result.phases.download.status = Status::pending;
        result.phases.upload.status = Status::pending;
        break;
    case PhaseSelection::ping:
        result.phases.ping.status = Status::pending;
        break;
    case PhaseSelection::download:
        result.phases.download.status = Status::pending;
        break;
    case PhaseSelection::upload:
        result.phases.upload.status = Status::pending;
        break;
    }
    return result;
}

PhaseResult empty_phase(Status status) {
    PhaseResult phase;
    phase.status = status;
    return phase;
}

PhaseResult ping_phase(const service::PingResult& value) {
    PhaseResult phase = empty_phase(Status::ok);
    phase.value_ms = round2(value.value_ms);
    phase.min_ms = round2(value.min_ms);
    phase.median_ms = round2(value.median_ms);
    phase.average_ms = round2(value.avg_ms);
    phase.jitter_ms = round2(value.jitter_ms);
    phase.samples = value.samples;
    phase.method = value.method;
    return phase;
}

PhaseResult throughput_phase(const service::ThroughputResult& value, const Error& error) {
    PhaseResult phase = empty_phase(Status::error);
    if (value.bytes > 0 || value.elapsed > std::chrono::nanoseconds::zero() ||
        value.successful_connections > 0 || value.failed_connections > 0) {
        phase.bytes = value.bytes;
        phase.successful_streams = value.successful_connections;
        phase.failed_streams = value.failed_connections;
    }
    append_unique(phase.warnings, value.warnings);
    if (value.elapsed > std::chrono::nanoseconds::zero()) {
        phase.elapsed_ms = round2(static_cast<double>(value.elapsed.count()) / 1e6);
    }
    if (!error) {
        phase.status = Status::ok;
        phase.mbps = round2(value.mbps);
    }
    return phase;
}

ErrorResult phase_error(service::Phase phase, const Error& error) {
    ErrorResult result;
    result.code = ErrorCode::unavailable;
    result.phase = phase;
    result.message = human_error(error);
    if (const auto* operation = error.as<service::OpError>()) {
        result.code = operation->code;
        result.retryable = operation->retryable;
        result.phase = operation->phase;
    } else if (error.is(errors::canceled_tag)) {
        result.code = ErrorCode::canceled;
    } else if (error.is(errors::deadline_exceeded_tag)) {
        result.code = ErrorCode::timeout;
        result.retryable = true;
    } else if (const auto* network = error.as<NetworkError>()) {
        result.retryable = true;
        if (network->timeout) {
            result.code = ErrorCode::timeout;
        }
    }
    return result;
}

void set_phase_error(MeasurementResult& result, PhaseResult& phase, service::Phase phase_id,
                     const Error& error) {
    phase.status = error.is(errors::canceled_tag) ? Status::canceled : Status::error;
    phase.error = phase_error(phase_id, error);
    if (!result.error) {
        result.error = phase.error;
    }
}

Status measurement_status(const MeasurementResult& result) {
    if (result.phases.select.status == Status::canceled) {
        return Status::canceled;
    }
    if (result.phases.select.status == Status::error) {
        return Status::error;
    }
    int succeeded = 0;
    int failed = 0;
    for (const PhaseResult* phase :
         {&result.phases.ping, &result.phases.download, &result.phases.upload}) {
        switch (phase->status) {
        case Status::ok:
            ++succeeded;
            break;
        case Status::error:
            ++failed;
            break;
        case Status::canceled:
            return Status::canceled;
        default:
            break;
        }
    }
    if (failed == 0) {
        return Status::ok;
    }
    return succeeded > 0 ? Status::partial : Status::error;
}

Status aggregate_status(const std::vector<MeasurementResult>& results) {
    if (results.empty()) {
        return Status::error;
    }
    int ok = 0;
    int failed = 0;
    for (const auto& result : results) {
        switch (result.status) {
        case Status::canceled:
            return Status::canceled;
        case Status::ok:
            ++ok;
            break;
        case Status::partial:
            ++ok;
            ++failed;
            break;
        default:
            ++failed;
            break;
        }
    }
    if (failed == 0) {
        return Status::ok;
    }
    return ok > 0 ? Status::partial : Status::error;
}

std::string human_error(const Error& error) {
    if (!error) {
        return "неизвестная ошибка";
    }
    if (error.is(errors::canceled_tag)) {
        return "операция отменена";
    }
    if (error.is(errors::deadline_exceeded_tag)) {
        return "истекло время ожидания";
    }
    if (const auto* operation = error.as<service::OpError>()) {
        switch (operation->code) {
        case ErrorCode::canceled:
            return "операция отменена";
        case ErrorCode::timeout:
            return "истекло время ожидания";
        case ErrorCode::protocol:
            return "ответ сервиса не соответствует протоколу";
        case ErrorCode::auth:
            return "сервис отклонил авторизацию";
        case ErrorCode::unavailable:
            return "сервис временно недоступен";
        case ErrorCode::internal:
            return "внутренняя ошибка";
        }
    }
    if (const auto* network = error.as<NetworkError>(); network && network->timeout) {
        return "истекло время ожидания";
    }
    return error.message();
}

void append_unique(std::vector<std::string>& destination, const std::vector<std::string>& values) {
    for (const auto& value : values) {
        if (!value.empty() &&
            std::find(destination.begin(), destination.end(), value) == destination.end()) {
            destination.push_back(value);
        }
    }
}

std::optional<std::string> non_empty(std::string value) {
    if (value.empty()) {
        return std::nullopt;
    }
    return value;
}

double round2(double value) {
    return std::round(value * 100) / 100;
}

boost::json::value to_json(const Envelope& envelope) {
    boost::json::object object;
    object["schema_version"] = schema_version;
    object["command"] = to_string(envelope.command);
    object["status"] = service::to_string(envelope.status);
    object["connection"] =
        envelope.connection ? connection_json(*envelope.connection) : boost::json::value(nullptr);
    boost::json::array results;
    for (const auto& result : envelope.results) {
        results.push_back(measurement_json(result));
    }
    object["results"] = std::move(results);
    return object;
}

Result<std::string> encode_json(const Envelope& envelope) {
    return json::encode_indented(to_json(envelope));
}

} // namespace puls::app
