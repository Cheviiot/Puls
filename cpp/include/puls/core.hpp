#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>

namespace puls {

enum class Command { Measure, IP, GUI, Help, Version };

struct Config {
    Command command = Command::Measure;
    QString service;
    QString profile = "balanced";
    int durationSeconds = 10;
    int connections = 0;
    QString only = "all";
    QString server;
    bool explicitService = false;
    bool showIP = false;
    bool json = false;
    bool verbose = false;
    bool noColor = false;
};

struct Cancellation {
    std::atomic_bool requested{false};
    void cancel() noexcept { requested.store(true); }
    bool canceled() const noexcept { return requested.load(); }
};

class OpError : public std::runtime_error {
public:
    QString code;
    QString phase;
    QString message;
    bool retryable;
    OpError(QString code, QString phase, QString message, bool retryable = false);
    QJsonObject json() const;
};

void checkCanceled(const Cancellation& cancellation, const QString& phase);
QJsonObject emptyPhase(const QString& status = "skipped");
QJsonObject pingPhase(const QList<double>& samples, const QString& method,
                      bool minimumPrimary = false);
QJsonObject failedPhase(const OpError& error);
QString displayName(const QString& service);
Config parseConfig(const QStringList& args);
QString helpText();
QString versionText();
using Observer = std::function<void(const QJsonObject&)>;

// All network objects must be constructed and destroyed in the calling thread.
// A backend is owned by exactly one Runner invocation; credentials stay in memory.
class Backend {
public:
    virtual ~Backend() = default;
    virtual QString id() const = 0;
    virtual QJsonObject select(const Config&, const Cancellation&) = 0;
    virtual QJsonObject ping(const Config&, const Cancellation&) = 0;
    virtual QJsonObject download(const Config&, const Cancellation&) = 0;
    virtual QJsonObject upload(const Config&, const Cancellation&) = 0;
    virtual QJsonObject download(const Config& config, const Cancellation& cancellation, Observer) {
        return download(config, cancellation);
    }
    virtual QJsonObject upload(const Config& config, const Cancellation& cancellation, Observer) {
        return upload(config, cancellation);
    }
    virtual QJsonObject connection(const Cancellation&) = 0;
};

using BackendFactory = std::function<std::unique_ptr<Backend>(const QString&)>;
std::unique_ptr<Backend> makeBackend(const QString& service);

class Runner {
public:
    explicit Runner(BackendFactory factory = makeBackend);
    QJsonObject run(const Config&, const Cancellation&, Observer observer = {}) const;
private:
    BackendFactory factory_;
};

int exitCode(const QJsonObject& envelope);
QString renderHuman(const QJsonObject& envelope);

} // namespace puls
