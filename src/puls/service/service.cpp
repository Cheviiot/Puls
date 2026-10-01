#include "puls/service/service.hpp"

#include "puls/core/text.hpp"

#include <algorithm>
#include <cmath>
#include <memory>

namespace puls::service {

namespace {

using namespace std::chrono_literals;

bool valid_latency(double sample) {
    return sample >= 0 && std::isfinite(sample);
}

std::vector<double> valid_latency_samples(const std::vector<double>& samples) {
    std::vector<double> valid;
    valid.reserve(samples.size());
    for (const double sample : samples) {
        if (valid_latency(sample)) {
            valid.push_back(sample);
        }
    }
    return valid;
}

double midpoint(double left, double right) {
    return left + (right - left) / 2;
}

double sorted_median(const std::vector<double>& sorted) {
    const std::size_t middle = sorted.size() / 2;
    if (sorted.size() % 2 == 0) {
        return midpoint(sorted[middle - 1], sorted[middle]);
    }
    return sorted[middle];
}

} // namespace

std::string_view to_string(ServiceId id) noexcept {
    switch (id) {
    case ServiceId::yandex:
        return "yandex";
    case ServiceId::speedtest:
        return "speedtest";
    case ServiceId::all:
        return "all";
    }
    return "";
}

std::string_view to_string(Phase phase) noexcept {
    switch (phase) {
    case Phase::select:
        return "select";
    case Phase::connection:
        return "connection";
    case Phase::ping:
        return "ping";
    case Phase::download:
        return "download";
    case Phase::upload:
        return "upload";
    }
    return "";
}

std::string_view to_string(Status status) noexcept {
    switch (status) {
    case Status::pending:
        return "pending";
    case Status::skipped:
        return "skipped";
    case Status::ok:
        return "ok";
    case Status::partial:
        return "partial";
    case Status::error:
        return "error";
    case Status::canceled:
        return "canceled";
    }
    return "";
}

std::string_view to_string(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::unavailable:
        return "unavailable";
    case ErrorCode::timeout:
        return "timeout";
    case ErrorCode::protocol:
        return "protocol";
    case ErrorCode::auth:
        return "auth";
    case ErrorCode::canceled:
        return "canceled";
    case ErrorCode::internal:
        return "internal";
    }
    return "";
}

std::optional<ServiceId> parse_service_id(std::string_view text) {
    const std::string value = text::to_lower_ascii(text::trim_space(text));
    if (value == "yandex") {
        return ServiceId::yandex;
    }
    if (value == "speedtest") {
        return ServiceId::speedtest;
    }
    if (value == "all") {
        return ServiceId::all;
    }
    return std::nullopt;
}

std::string display_name(ServiceId id) {
    switch (id) {
    case ServiceId::yandex:
        return "Яндекс";
    case ServiceId::speedtest:
        return "speedtest.ru";
    case ServiceId::all:
        break;
    }
    return std::string(to_string(id));
}

std::string phase_display_name(Phase phase) {
    switch (phase) {
    case Phase::select:
        return "выбор сервера";
    case Phase::connection:
        return "данные подключения";
    case Phase::ping:
        return "задержка";
    case Phase::download:
        return "скачивание";
    case Phase::upload:
        return "отдача";
    }
    return std::string(to_string(phase));
}

Error new_error(ServiceId service, Phase phase, ErrorCode code, bool retryable, Error cause) {
    if (code == ErrorCode::canceled || cause.is(errors::canceled_tag)) {
        retryable = false;
    }
    std::string prefix = display_name(service) + ": " + phase_display_name(phase);
    if (!cause) {
        prefix += ": ";
        prefix += to_string(code);
    }
    return Error::with_detail(std::make_shared<OpError>(service, phase, code, retryable),
                              std::move(prefix), std::move(cause));
}

Error http_status_error(int status_code, std::string status, std::string operation) {
    std::string message = operation + " вернул состояние " + status;
    return Error::with_detail(
        std::make_shared<HttpStatusError>(status_code, std::move(status), std::move(operation)),
        std::move(message));
}

const ErrorTag protocol_tag{"service.ErrProtocol"};
const ErrorTag authorization_tag{"service.ErrAuthorization"};

Error protocol_error(Error cause) {
    return Error::tagged(protocol_tag, "ошибка протокола", std::move(cause));
}

Error protocol_error(std::string message) {
    return protocol_error(Error::make(std::move(message)));
}

Error authorization_error(Error cause) {
    return Error::tagged(authorization_tag, "ошибка авторизации", std::move(cause));
}

Error authorization_error(std::string message) {
    return authorization_error(Error::make(std::move(message)));
}

ErrorCode classify_error(const Error& error) {
    if (error.is(errors::canceled_tag)) {
        return ErrorCode::canceled;
    }
    if (error.is(errors::deadline_exceeded_tag)) {
        return ErrorCode::timeout;
    }
    if (error.is(authorization_tag)) {
        return ErrorCode::auth;
    }
    if (error.is(protocol_tag)) {
        return ErrorCode::protocol;
    }
    if (const auto* status = error.as<HttpStatusError>()) {
        return classify_http_status(status->status_code).first;
    }
    if (const auto* network = error.as<NetworkError>(); network && network->timeout) {
        return ErrorCode::timeout;
    }
    return ErrorCode::unavailable;
}

std::pair<ErrorCode, bool> classify_http_status(int status_code) {
    if (status_code == 401 || status_code == 403) {
        return {ErrorCode::auth, true};
    }
    if (status_code == 408 || status_code == 429 || status_code >= 500) {
        return {ErrorCode::unavailable, true};
    }
    return {ErrorCode::protocol, false};
}

bool retryable_code(ErrorCode code) {
    return code == ErrorCode::unavailable || code == ErrorCode::timeout || code == ErrorCode::auth;
}

Result<std::pair<int, int>> connection_limits(const MeasurementConfig& config, int native) {
    if (config.duration < 3s || config.duration > 60s) {
        return Error::make("длительность должна быть от 3 до 60 секунд");
    }
    const int maximum = config.max_connections == 0 ? 16 : config.max_connections;
    if (maximum < 1 || maximum > 16) {
        return Error::make("предельное число потоков должно быть от 1 до 16");
    }
    int initial = config.connections;
    if (initial == 0) {
        initial = std::max(1, std::min(native, maximum));
    }
    if (initial < 1 || initial > maximum) {
        return Error::make("число потоков должно быть от 1 до " + std::to_string(maximum));
    }
    return std::pair{initial, maximum};
}

int adaptive_connections(double mbps) {
    if (mbps > 300) {
        return 16;
    }
    if (mbps > 50) {
        return 12;
    }
    return 8;
}

measure::ProgressFn adapt_progress(const ProgressFn& progress) {
    if (!progress) {
        return nullptr;
    }
    return [progress](const measure::RunProgress& value) {
        progress(ThroughputProgress{value.mbps, value.bytes, value.elapsed, value.active});
    };
}

ThroughputResult convert_run_result(const measure::RunResult& result) {
    ThroughputResult converted;
    std::size_t worker_errors = 0;
    for (const auto& error : result.worker_errors) {
        if (error) {
            ++worker_errors;
        }
    }
    if (worker_errors > 0) {
        converted.warnings.push_back("сетевые сбои отдельных потоков: " +
                                     std::to_string(worker_errors));
    }
    converted.mbps = measure::mbps(result.bytes, result.elapsed);
    converted.bytes = result.bytes;
    converted.elapsed = result.elapsed;
    converted.successful_connections = result.workers_ok;
    converted.failed_connections = result.workers_failed;
    return converted;
}

std::function<void(int, int, const Error&)> worker_error_logger(const LogFunc& log,
                                                                ServiceId service, Phase phase) {
    if (!log) {
        return nullptr;
    }
    return [log, service, phase](int index, int attempt, const Error& error) {
        const std::string reconnect = attempt == 0 ? "да" : "нет";
        log(display_name(service) + " · " + phase_display_name(phase) +
            ": поток=" + std::to_string(index + 1) + ", попытка=" + std::to_string(attempt + 1) +
            ", переподключение=" + reconnect + ": " + error.message());
    };
}

PingResult stats(const std::vector<double>& samples_ms) {
    std::vector<double> valid = valid_latency_samples(samples_ms);
    if (valid.empty()) {
        return {};
    }
    std::sort(valid.begin(), valid.end());
    const double minimum = valid.front();
    const double median = sorted_median(valid);

    double average = 0;
    int count = 0;
    for (const double sample : samples_ms) {
        if (!valid_latency(sample)) {
            continue;
        }
        ++count;
        average += (sample - average) / count;
    }

    double jitter = 0;
    int jitter_samples = 0;
    double previous = 0;
    bool has_previous = false;
    for (const double sample : samples_ms) {
        if (!valid_latency(sample)) {
            continue;
        }
        if (has_previous) {
            ++jitter_samples;
            jitter += (std::fabs(sample - previous) - jitter) / jitter_samples;
        }
        previous = sample;
        has_previous = true;
    }

    PingResult result;
    result.value_ms = average;
    result.min_ms = minimum;
    result.median_ms = median;
    result.avg_ms = average;
    result.jitter_ms = jitter;
    result.samples = static_cast<int>(valid.size());
    result.method = "average";
    return result;
}

PingResult stats_with_method(const std::vector<double>& samples_ms, std::string_view method) {
    PingResult result = stats(samples_ms);
    if (result.samples == 0) {
        return result;
    }
    if (method == "minimum") {
        result.method = "minimum";
        result.value_ms = result.min_ms;
    } else if (method == "median") {
        result.method = "median";
        result.value_ms = result.median_ms;
    } else {
        result.method = "average";
        result.value_ms = result.avg_ms;
    }
    return result;
}

double median_absolute_deviation(const std::vector<double>& samples_ms) {
    std::vector<double> values = valid_latency_samples(samples_ms);
    if (values.size() < 2) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const double median = sorted_median(values);
    std::vector<double> deviations;
    deviations.reserve(values.size());
    for (const double value : values) {
        deviations.push_back(std::fabs(value - median));
    }
    std::sort(deviations.begin(), deviations.end());
    return sorted_median(deviations);
}

} // namespace puls::service
