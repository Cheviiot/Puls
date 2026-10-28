#pragma once

#include "puls/core.hpp"
#include <chrono>
#include <optional>

namespace puls::transfer {

using Milliseconds = std::chrono::milliseconds;
using Progress = std::function<void(const QJsonObject&)>;

struct RunConfig {
    QString phase;
    Milliseconds duration{10000};
    Milliseconds warmup{0};
    Milliseconds startupTimeout{5000};
    Milliseconds readyGrace{300};
    Milliseconds reconnectDelay{200};
    Milliseconds progressInterval{200};
    int initialWorkers = 1;
    int maxWorkers = 16;
    int reconnects = 1;
    std::function<int(double)> adaptive;
};

// Each attempt owns its network objects in its worker thread. Poll stopped()
// while waiting for I/O and abort that I/O immediately when it becomes true.
// ready() follows protocol readiness; record() accepts confirmed bytes only.
struct WorkerContext {
    std::function<bool()> stopped;
    std::function<void()> ready;
    std::function<void(qint64)> record;
};
using Worker = std::function<void(int index, int attempt, const WorkerContext&)>;

struct RunResult {
    qint64 bytes = 0;
    double elapsedSeconds = 0;
    int workersOK = 0;
    int workersFailed = 0;
    QList<OpError> workerErrors;
    std::optional<OpError> error;
};

RunResult run(const RunConfig&, const Cancellation&, const Worker&, Progress = {});
QJsonObject phaseResult(const RunResult&, const QString& method);
RunConfig configure(const Config&, const QString& phase, int nativeWorkers,
                    Milliseconds warmup, Milliseconds startupTimeout,
                    Milliseconds readyGrace);
int adaptiveConnections(double mbps);
double mbps(qint64 bytes, double elapsedSeconds);

} // namespace puls::transfer
