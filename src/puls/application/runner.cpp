#include "puls/application/runner.hpp"

#include "puls/core/text.hpp"
#include "puls/service/speedtestru/backend.hpp"
#include "puls/service/yandex/backend.hpp"

namespace puls::app {

namespace {

using namespace std::chrono_literals;
using service::ErrorCode;
using service::Phase;
using service::ServiceId;
using service::Status;

constexpr auto phase_attempt_timeout = 15s;
constexpr auto connection_timeout = 15s;

void emit(const Observer& observer, const RunEvent& event) {
    if (!observer) {
        return;
    }
    try {
        observer(event);
    } catch (...) {
        // A broken observer must not break the measurement.
    }
}

RunEvent event(EventKind kind, std::optional<ServiceId> service = std::nullopt,
               std::optional<Phase> phase = std::nullopt) {
    RunEvent value;
    value.kind = kind;
    value.service = service;
    value.phase = phase;
    return value;
}

void skip_pending_phases(MeasurementResult& result) {
    for (PhaseResult* phase :
         {&result.phases.ping, &result.phases.download, &result.phases.upload}) {
        if (phase->status == Status::pending) {
            phase->status = Status::skipped;
        }
    }
}

} // namespace

Error with_retry(const Context& ctx, int attempts, const std::function<Error()>& operation) {
    if (attempts < 1) {
        return Error::make("число попыток должно быть положительным");
    }
    Error last;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        if (Error error = ctx.err()) {
            return error;
        }
        last = operation();
        if (!last) {
            return {};
        }
        if (const auto* operation_error = last.as<service::OpError>();
            operation_error && !operation_error->retryable) {
            return last;
        }
        if (attempt == attempts - 1) {
            break;
        }
        if (ctx.wait_for(300ms * (attempt + 1))) {
            return ctx.err();
        }
    }
    return last;
}

Error validate_measure_request(const MeasureRequest& request) {
    const auto duration = request.duration == std::chrono::nanoseconds::zero()
                              ? std::chrono::nanoseconds(profile_duration(request.profile))
                              : request.duration;
    if (duration < 3s || duration > 60s) {
        return Error::make("длительность должна быть от 3 до 60 секунд");
    }
    if (request.connections < 0 || request.connections > 16) {
        return Error::make("число соединений должно быть 0 (авто) или от 1 до 16");
    }
    if (!text::trim_space(request.server).empty() && request.service != ServiceId::speedtest) {
        return Error::make("ручной сервер можно использовать только с сервисом speedtest");
    }
    return {};
}

Runner::Runner(RunnerOptions options) : options_(std::move(options)) {
    const auto tls = options_.tls;
    if (!options_.yandex_factory) {
        options_.yandex_factory = [tls](const std::string&, const service::LogFunc& log) {
            yandex::Options backend_options;
            backend_options.log = log;
            backend_options.tls = tls;
            return std::make_shared<yandex::Backend>(std::move(backend_options));
        };
    }
    if (!options_.speedtest_factory) {
        options_.speedtest_factory = [tls](const std::string& server, const service::LogFunc& log) {
            speedtestru::Options backend_options;
            backend_options.server = server;
            backend_options.log = log;
            backend_options.tls = tls;
            return std::make_shared<speedtestru::Backend>(std::move(backend_options));
        };
    }
}

void Runner::log(std::string_view message) const {
    if (options_.log) {
        options_.log(message);
    }
}

std::shared_ptr<service::Backend> Runner::backend(ServiceId id, const std::string& server) const {
    if (id == ServiceId::speedtest) {
        return options_.speedtest_factory(server, options_.log);
    }
    return options_.yandex_factory({}, options_.log);
}

Envelope Runner::measure(const Context& ctx, MeasureRequest request,
                         const Observer& observer) const {
    Envelope envelope = new_envelope(Command::measure);
    if (request.duration == std::chrono::nanoseconds::zero()) {
        request.duration = profile_duration(request.profile);
    }
    emit(observer, event(EventKind::run_started));

    if (request.show_connection) {
        ConnectionRequest connection;
        connection.explicit_service = request.service != ServiceId::all;
        if (connection.explicit_service) {
            connection.service = request.service;
        }
        envelope.connection = detect_connection(ctx, connection, observer);
    }

    std::vector<ServiceId> services;
    switch (request.service) {
    case ServiceId::speedtest:
        services = {ServiceId::speedtest};
        break;
    case ServiceId::all:
        services = {ServiceId::yandex, ServiceId::speedtest};
        break;
    case ServiceId::yandex:
        services = {ServiceId::yandex};
        break;
    }
    for (const ServiceId id : services) {
        const auto instance =
            backend(id, request.service == ServiceId::speedtest ? request.server : std::string());
        envelope.results.push_back(measure_service(ctx, *instance, request, observer));
        if (ctx.done()) {
            break;
        }
    }
    envelope.status = aggregate_status(envelope.results);
    if (ctx.cause() == CancelCause::canceled) {
        envelope.status = Status::canceled;
    }
    RunEvent completed = event(EventKind::run_completed);
    completed.envelope = envelope;
    emit(observer, completed);
    return envelope;
}

ConnectionResult Runner::detect_connection(const Context& ctx, const ConnectionRequest& request,
                                           const Observer& observer) const {
    emit(observer, event(EventKind::connection_started));
    ConnectionResult result;
    result.status = Status::pending;
    std::vector<ServiceId> services;
    if (request.explicit_service && request.service) {
        services = {*request.service};
    } else {
        services = {ServiceId::speedtest, ServiceId::yandex};
    }

    const auto complete = [&observer](const ConnectionResult& value,
                                      std::optional<ServiceId> service) {
        RunEvent completed = event(EventKind::connection_completed, service);
        completed.connection = value;
        emit(observer, completed);
    };

    std::vector<Error> failures;
    for (std::size_t index = 0; index < services.size(); ++index) {
        const ServiceId id = services[index];
        const auto instance = backend(id, {});
        auto* detector = dynamic_cast<service::ConnectionInfoBackend*>(instance.get());
        if (detector == nullptr) {
            failures.push_back(
                service::new_error(id, Phase::connection, ErrorCode::internal, false,
                                   Error::make("сервис не поддерживает определение подключения")));
            continue;
        }
        Result<service::ConnectionInfo> info = [&] {
            const CancelScope attempt(ctx, connection_timeout);
            return detector->detect_connection(attempt.context());
        }();
        if (info && !info->external_ip.valid()) {
            info = service::new_error(id, Phase::connection, ErrorCode::protocol, false,
                                      Error::make("сервис не вернул действительный IP-адрес"));
        }
        if (!info) {
            const Error& error = info.error();
            failures.push_back(error);
            log("данные подключения через " + service::display_name(id) + ": " + error.message());
            if (error.is(errors::canceled_tag) || ctx.cause() == CancelCause::canceled) {
                result.status = Status::canceled;
                result.error = phase_error(Phase::connection, errors::canceled());
                complete(result, std::nullopt);
                return result;
            }
            continue;
        }

        result.status = Status::ok;
        result.external_ip = info->external_ip.to_string();
        result.isp = non_empty(info->isp);
        result.detected_by = id;
        append_unique(result.warnings, info->warnings);
        for (const auto& warning : info->warnings) {
            log("данные подключения через " + service::display_name(id) + ": " + warning);
        }
        if (index > 0) {
            append_unique(result.warnings,
                          {"использован резервный сервис " + service::display_name(id)});
            log("fallback определения подключения: " + service::display_name(id));
        }
        complete(result, id);
        return result;
    }

    Error error = Error::join(std::move(failures));
    if (!error) {
        error = Error::make("нет доступного сервиса определения подключения");
    }
    result.status = Status::error;
    result.error = phase_error(Phase::connection, error);
    complete(result, std::nullopt);
    return result;
}

MeasurementResult Runner::measure_service(const Context& ctx, service::Backend& backend,
                                          const MeasureRequest& request,
                                          const Observer& observer) const {
    const ServiceId id = backend.id();
    MeasurementResult result = new_measurement_result(id, request.only);
    emit(observer, event(EventKind::service_started, id));
    emit(observer, event(EventKind::phase_started, id, Phase::select));

    service::Server server;
    const Error select_error = with_retry(ctx, 2, [&]() -> Error {
        const CancelScope attempt(ctx, phase_attempt_timeout);
        auto selected = backend.select_server(attempt.context());
        if (!selected) {
            return selected.error();
        }
        server = std::move(selected).value();
        return {};
    });
    const auto complete_phase = [&](Phase phase, const PhaseResult& value) {
        RunEvent completed = event(EventKind::phase_completed, id, phase);
        completed.phase_result = value;
        emit(observer, completed);
    };
    const auto complete_service = [&] {
        RunEvent completed = event(EventKind::service_completed, id);
        completed.measurement = result;
        emit(observer, completed);
    };
    if (select_error) {
        log("сервис=" + std::string(service::to_string(id)) +
            " этап=select: " + select_error.message());
        set_phase_error(result, result.phases.select, Phase::select, select_error);
        skip_pending_phases(result);
        result.status = measurement_status(result);
        complete_phase(Phase::select, result.phases.select);
        complete_service();
        return result;
    }
    result.phases.select.status = Status::ok;
    result.server = ServerResult{server.name, non_empty(server.city), non_empty(server.region)};
    log("сервис=" + std::string(service::to_string(id)) + " сервер измерения=" + server.name);
    RunEvent selected = event(EventKind::server_selected, id, Phase::select);
    selected.server = server;
    emit(observer, selected);
    complete_phase(Phase::select, result.phases.select);

    if (result.phases.ping.status == Status::pending) {
        emit(observer, event(EventKind::phase_started, id, Phase::ping));
        service::PingResult ping;
        const Error ping_error = with_retry(ctx, 2, [&]() -> Error {
            const CancelScope attempt(ctx, phase_attempt_timeout);
            auto measured = backend.ping(attempt.context());
            if (!measured) {
                return measured.error();
            }
            ping = std::move(measured).value();
            return {};
        });
        if (ping_error) {
            log("сервис=" + std::string(service::to_string(id)) +
                " этап=ping: " + ping_error.message());
            set_phase_error(result, result.phases.ping, Phase::ping, ping_error);
        } else {
            result.phases.ping = ping_phase(ping);
            RunEvent completed = event(EventKind::ping_completed, id, Phase::ping);
            completed.ping = ping;
            emit(observer, completed);
        }
        complete_phase(Phase::ping, result.phases.ping);
    }

    run_throughput(ctx, backend, request, Phase::download, result, observer);
    run_throughput(ctx, backend, request, Phase::upload, result, observer);

    result.status = measurement_status(result);
    complete_service();
    return result;
}

void Runner::run_throughput(const Context& ctx, service::Backend& backend,
                            const MeasureRequest& request, Phase phase, MeasurementResult& result,
                            const Observer& observer) const {
    const ServiceId id = backend.id();
    PhaseResult& destination =
        phase == Phase::download ? result.phases.download : result.phases.upload;
    const service::Capability capability =
        phase == Phase::download ? service::Capability::download : service::Capability::upload;
    if (destination.status != Status::pending) {
        return;
    }
    const auto complete = [&] {
        RunEvent completed = event(EventKind::phase_completed, id, phase);
        completed.phase_result = destination;
        emit(observer, completed);
    };
    if (Error error = ctx.err()) {
        set_phase_error(result, destination, phase, error);
        complete();
        return;
    }
    if (!service::has(backend.capabilities(), capability)) {
        destination.status = Status::skipped;
        complete();
        return;
    }

    emit(observer, event(EventKind::phase_started, id, phase));
    const service::MeasurementConfig config{request.duration, request.connections, 16};
    const service::ProgressFn progress = [&observer, id,
                                          phase](const service::ThroughputProgress& value) {
        RunEvent update = event(EventKind::throughput_progress, id, phase);
        update.throughput = value;
        emit(observer, update);
    };
    const service::ThroughputOutcome measured = phase == Phase::download
                                                    ? backend.download(ctx, config, progress)
                                                    : backend.upload(ctx, config, progress);
    destination = throughput_phase(measured.result, measured.error);
    append_unique(result.warnings, measured.result.warnings);
    if (measured.error) {
        log("сервис=" + std::string(service::to_string(id)) +
            " этап=" + std::string(service::to_string(phase)) + ": " + measured.error.message());
        set_phase_error(result, destination, phase, measured.error);
    }
    complete();
}

} // namespace puls::app
