#include "puls/core.hpp"

#include <QHostAddress>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <utility>

namespace puls {

OpError::OpError(QString codeValue, QString phaseValue, QString text, bool canRetry)
    : std::runtime_error(text.toStdString()), code(std::move(codeValue)),
      phase(std::move(phaseValue)), message(std::move(text)),
      retryable(canRetry && code != "canceled") {}

QJsonObject OpError::json() const {
    return {{"code", code}, {"phase", phase}, {"message", message}, {"retryable", retryable}};
}

void checkCanceled(const Cancellation& cancellation, const QString& phase) {
    if (cancellation.canceled())
        throw OpError("canceled", phase, QStringLiteral("операция отменена"));
}

QJsonObject emptyPhase(const QString& status) {
    QJsonObject result{{"status", status}, {"warnings", QJsonArray{}}};
    for (const auto* key : {"value_ms", "min_ms", "median_ms", "average_ms", "jitter_ms",
                            "samples", "method", "mbps", "bytes", "elapsed_ms",
                            "successful_streams", "failed_streams", "error"})
        result.insert(key, QJsonValue::Null);
    return result;
}

QJsonObject failedPhase(const OpError& error) {
    auto result = emptyPhase(error.code == "canceled" ? "canceled" : "error");
    for (const auto* key : {"bytes", "elapsed_ms", "successful_streams", "failed_streams", "warnings"})
        if (error.transferDiagnostics.contains(key)) result.insert(key, error.transferDiagnostics.value(key));
    result.insert("error", error.json());
    return result;
}

QJsonObject pingPhase(const QList<double>& samples, const QString& requestedMethod, bool minimumPrimary) {
    QList<double> valid;
    double average = 0, jitter = 0, previous = 0;
    int count = 0;
    for (double sample : samples) {
        if (!std::isfinite(sample) || sample < 0)
            continue;
        valid.append(sample);
        ++count;
        average += (sample - average) / count;
        if (count > 1)
            jitter += (std::abs(sample - previous) - jitter) / (count - 1);
        previous = sample;
    }
    if (valid.isEmpty())
        throw OpError("protocol", "ping", QStringLiteral("сервис не вернул допустимых измерений задержки"));
    std::sort(valid.begin(), valid.end());
    double median = valid[valid.size() / 2];
    if (valid.size() % 2 == 0)
        median = valid[valid.size() / 2 - 1] + (median - valid[valid.size() / 2 - 1]) / 2;
    QString method = minimumPrimary ? "minimum" : requestedMethod;
    double primary = average;
    if (method == "minimum") primary = valid.front();
    else if (method == "median") primary = median;
    else method = "average";
    auto round = [](double value) { return std::round(value * 100) / 100; };
    auto result = emptyPhase("ok");
    result.insert("value_ms", round(primary));
    result.insert("min_ms", round(valid.front()));
    result.insert("median_ms", round(median));
    result.insert("average_ms", round(average));
    result.insert("jitter_ms", round(jitter));
    result.insert("samples", count);
    result.insert("method", method);
    return result;
}

QString displayName(const QString& id) {
    if (id == "yandex") return QStringLiteral("Яндекс.Интернетометр");
    if (id == "speedtest") return "speedtest.ru";
    return QStringLiteral("оба сервиса");
}

namespace {

QString aggregate(const QJsonArray& results) {
    if (results.isEmpty()) return "error";
    int successful = 0, failed = 0;
    for (const auto& item : results) {
        const auto status = item.toObject().value("status").toString();
        if (status == "canceled") return "canceled";
        if (status == "ok" || status == "partial") ++successful;
        if (status != "ok") ++failed;
    }
    if (!failed) return "ok";
    return successful ? "partial" : "error";
}

QString measurementStatus(const QJsonObject& phases) {
    const auto selected = phases.value("select").toObject().value("status").toString();
    if (selected == "error" || selected == "canceled") return selected;
    QJsonArray statuses;
    for (const auto* phase : {"ping", "download", "upload"}) {
        const auto value = phases.value(phase).toObject();
        if (value.value("status") != "skipped") statuses.append(value);
    }
    return aggregate(statuses);
}

QJsonObject internalError(const QString& phase) {
    return failedPhase(OpError("internal", phase, QStringLiteral("внутренняя ошибка измерения")));
}

OpError reportedError(const QJsonObject& value, const QString& phase) {
    const auto detail = value.value("error").toObject();
    QString code = detail.value("code").toString();
    if (value.value("status") == "canceled") code = "canceled";
    if (!QStringList{"unavailable", "timeout", "protocol", "auth", "canceled", "internal"}.contains(code))
        code = "protocol";
    auto message = detail.value("message").toString();
    if (message.isEmpty()) message = QStringLiteral("сервис не вернул допустимый результат");
    return OpError(code, phase, message, detail.value("retryable").toBool());
}

QJsonObject retry(const Cancellation& cancellation, const QString& phase,
                  const std::function<QJsonObject()>& operation) {
    for (int attempt = 0; ; ++attempt) {
        checkCanceled(cancellation, phase);
        try { return operation(); }
        catch (const OpError& error) {
            if (!error.retryable || error.code == "canceled" || attempt == 1) throw;
            for (int pause = 0; pause < 5; ++pause) {
                checkCanceled(cancellation, phase);
                QThread::msleep(20);
            }
        }
    }
}

void validate(const Config& config) {
    if (config.command != Command::Measure && config.command != Command::IP)
        throw OpError("internal", "select", QStringLiteral("команда не поддерживает измерение"));
    if (!QStringList{"", "yandex", "speedtest", "all"}.contains(config.service) ||
        (config.command == Command::IP && config.service == "all"))
        throw OpError("internal", "select", QStringLiteral("неизвестный сервис измерения"));
    if (config.command == Command::IP) return;
    if (!QStringList{"quick", "balanced", "accurate"}.contains(config.profile) ||
        config.durationSeconds < 3 || config.durationSeconds > 60 ||
        config.connections < 0 || config.connections > 16 ||
        !QStringList{"all", "ping", "download", "upload"}.contains(config.only) ||
        (!config.server.isEmpty() && config.service != "speedtest"))
        throw OpError("internal", "select", QStringLiteral("недопустимые параметры измерения"));
}

} // namespace

Runner::Runner(BackendFactory factory) : factory_(std::move(factory)) {}

QJsonObject Runner::run(const Config& config, const Cancellation& cancellation, Observer observer) const {
    validate(config);
    QJsonObject envelope{{"schema_version", 1},
                         {"command", config.command == Command::IP ? "ip" : "measure"},
                         {"status", "error"}, {"connection", QJsonValue::Null},
                         {"results", QJsonArray{}}};
    auto emitEvent = [&](const QString& kind, QJsonObject event = {}) {
        if (observer) { event.insert("kind", kind); observer(event); }
    };
    emitEvent("run_started");
    auto detectConnection = [&]() {
        emitEvent("connection_started");
        QJsonObject result{{"status", "error"}, {"external_ip", QJsonValue::Null},
                           {"isp", QJsonValue::Null}, {"detected_by", QJsonValue::Null},
                           {"warnings", QJsonArray{}}, {"error", QJsonValue::Null}};
        QStringList ids{config.service.isEmpty() ? "yandex" : config.service};
        if ((config.command == Command::IP && !config.explicitService) || config.service == "all")
            ids = {"speedtest", "yandex"};
        for (qsizetype index = 0; index < ids.size(); ++index) {
            try {
                checkCanceled(cancellation, "connection");
                auto backend = factory_(ids[index]);
                if (!backend) throw OpError("internal", "connection", QStringLiteral("сервис недоступен"));
                auto connection = backend->connection(cancellation);
                QHostAddress address(connection.value("external_ip").toString());
                if (connection.value("status") != "ok")
                    throw reportedError(connection, "connection");
                if (address.isNull() || !address.scopeId().isEmpty())
                    throw OpError("protocol", "connection", QStringLiteral("сервис не вернул действительный IP-адрес"));
                for (const auto* field : {"external_ip", "isp"}) {
                    auto value = connection.value(field);
                    result.insert(field, value.isUndefined() || value.toString().isEmpty()
                                         ? QJsonValue(QJsonValue::Null) : value);
                }
                result.insert("external_ip", address.toString());
                result.insert("status", "ok");
                result.insert("detected_by", ids[index]);
                result.insert("error", QJsonValue::Null);
                auto warnings = connection.value("warnings").toArray();
                if (index > 0) warnings.append(QStringLiteral("использован резервный сервис %1").arg(displayName(ids[index])));
                result.insert("warnings", warnings);
                break;
            } catch (const OpError& error) {
                result.insert("error", error.json());
                if (error.code == "canceled") { result.insert("status", "canceled"); break; }
            } catch (const std::exception&) {
                result.insert("error", OpError("internal", "connection", QStringLiteral("внутренняя ошибка определения подключения")).json());
            }
        }
        emitEvent("connection_completed", {{"connection", result}});
        return result;
    };
    if (config.command == Command::IP || config.showIP) {
        const auto connection = detectConnection();
        envelope.insert("connection", connection);
        if (config.command == Command::IP) {
            envelope.insert("status", cancellation.canceled() ? "canceled" : connection.value("status").toString());
            emitEvent("run_completed", {{"envelope", envelope}});
            return envelope;
        }
    }
    const QStringList ids = config.service == "all" ? QStringList{"yandex", "speedtest"}
        : QStringList{config.service.isEmpty() ? "yandex" : config.service};
    QJsonArray results;
    for (const auto& id : ids) {
        QJsonObject phases{{"select", emptyPhase("pending")}, {"ping", emptyPhase()},
                           {"download", emptyPhase()}, {"upload", emptyPhase()}};
        QJsonObject result{{"service", id}, {"status", "error"}, {"server", QJsonValue::Null},
                           {"phases", phases}, {"error", QJsonValue::Null}, {"warnings", QJsonArray{}}};
        emitEvent("service_started", {{"service", id}});
        std::unique_ptr<Backend> backend;
        emitEvent("phase_started", {{"service", id}, {"phase", "select"}});
        try {
            checkCanceled(cancellation, "select");
            backend = factory_(id);
            if (!backend) throw OpError("internal", "select", QStringLiteral("сервис недоступен"));
            auto server = retry(cancellation, "select", [&] { return backend->select(config, cancellation); });
            if (server.value("name").toString().isEmpty())
                throw OpError("protocol", "select", QStringLiteral("сервис не вернул сервер измерения"));
            for (const auto* key : {"city", "region"})
                if (server.value(key).toString().isEmpty()) server.insert(key, QJsonValue::Null);
            result.insert("server", server);
            phases.insert("select", emptyPhase("ok"));
            emitEvent("server_selected", {{"service", id}, {"phase", "select"}, {"server", server}});
        } catch (const OpError& error) { phases.insert("select", failedPhase(error)); }
          catch (const std::exception&) { phases.insert("select", internalError("select")); }
        emitEvent("phase_completed", {{"service", id}, {"phase", "select"}, {"phase_result", phases.value("select")}});
        if (phases.value("select").toObject().value("status") == "ok") {
            bool phaseCanceled = false;
            for (const QString& phase : {QString("ping"), QString("download"), QString("upload")}) {
                if (config.only != "all" && config.only != phase) continue;
                emitEvent("phase_started", {{"service", id}, {"phase", phase}});
                QJsonObject value;
                try {
                    if (phaseCanceled)
                        throw OpError("canceled", phase, QStringLiteral("операция отменена"));
                    checkCanceled(cancellation, phase);
                    if (phase == "ping") value = retry(cancellation, phase, [&] { return backend->ping(config, cancellation); });
                    else {
                        auto progress = [&](const QJsonObject& throughput) {
                            emitEvent("throughput_progress", {{"service", id}, {"phase", phase}, {"throughput", throughput}});
                        };
                        value = phase == "download" ? backend->download(config, cancellation, progress)
                                                    : backend->upload(config, cancellation, progress);
                    }
                    if (value.value("status") == "ok") {
                        const auto measured = value.value(phase == "ping" ? "value_ms" : "mbps");
                        if (!measured.isDouble() || !std::isfinite(measured.toDouble()) || measured.toDouble() < 0)
                            throw OpError("protocol", phase, QStringLiteral("сервис не вернул допустимый результат"));
                    } else throw reportedError(value, phase);
                } catch (const OpError& error) { value = failedPhase(error); }
                  catch (const std::exception&) { value = internalError(phase); }
                phases.insert(phase, value);
                auto warnings = result.value("warnings").toArray();
                for (const auto& warning : value.value("warnings").toArray())
                    if (warning.isString() && !warning.toString().isEmpty() && !warnings.contains(warning))
                        warnings.append(warning);
                result.insert("warnings", warnings);
                emitEvent("phase_completed", {{"service", id}, {"phase", phase}, {"phase_result", value}});
                phaseCanceled = cancellation.canceled() || value.value("status") == "canceled";
            }
        }
        for (const auto* phase : {"select", "ping", "download", "upload"}) {
            const auto error = phases.value(phase).toObject().value("error");
            if (error.isObject()) { result.insert("error", error); break; }
        }
        result.insert("phases", phases);
        result.insert("status", measurementStatus(phases));
        results.append(result);
        emitEvent("service_completed", {{"service", id}, {"measurement", result}});
        if (cancellation.canceled() || result.value("status") == "canceled") break;
    }
    envelope.insert("results", results);
    envelope.insert("status", cancellation.canceled() ? "canceled" : aggregate(results));
    emitEvent("run_completed", {{"envelope", envelope}});
    return envelope;
}

int exitCode(const QJsonObject& envelope) {
    const auto status = envelope.value("status").toString();
    if (status == "ok") return 0;
    if (status == "canceled") return 130;
    return 1;
}

} // namespace puls
