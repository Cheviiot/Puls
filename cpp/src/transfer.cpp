#include "puls/transfer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

namespace puls::transfer {
namespace {
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

struct State {
    qint64 measured = 0;
    QList<OpError> errors;
    bool ready = false;
};

struct Shared {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic_bool stop{false};
    std::atomic<qint64> deadlineNanos{0};
    std::array<State, 16> states;
    qint64 bytes = 0;
    qint64 warmupBytes = 0;
    int ready = 0;
    int done = 0;
    int active = 0;
    bool measuring = false;
};

qint64 nanos(TimePoint time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

bool stopped(const Shared& shared, const Cancellation& cancellation) {
    const auto deadline = shared.deadlineNanos.load();
    return shared.stop.load() || cancellation.canceled() ||
        (deadline != 0 && nanos(Clock::now()) >= deadline);
}

// Even if a callback throws, all workers observe stop and are joined before
// their captured protocol state can be destroyed.
struct JoinWorkers {
    Shared& shared;
    std::vector<std::thread>& threads;
    ~JoinWorkers() {
        shared.stop.store(true);
        shared.changed.notify_all();
        for (auto& thread : threads) if (thread.joinable()) thread.join();
    }
};

void addBytes(qint64& target, qint64 count) {
    if (count > std::numeric_limits<qint64>::max() - target)
        throw OpError("protocol", "transfer", QStringLiteral("счётчик подтверждённых данных переполнен"));
    target += count;
}

double seconds(Clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}
} // namespace

double mbps(qint64 bytes, double elapsedSeconds) {
    if (bytes <= 0 || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0) return 0;
    return static_cast<double>(bytes) * 8 / 1e6 / elapsedSeconds;
}

int adaptiveConnections(double rate) {
    if (rate > 300) return 16;
    if (rate > 50) return 12;
    return 8;
}

RunConfig configure(const Config& config, const QString& phase, int nativeWorkers,
                    Milliseconds warmup, Milliseconds startupTimeout, Milliseconds readyGrace) {
    if (config.durationSeconds < 3 || config.durationSeconds > 60 ||
        config.connections < 0 || config.connections > 16)
        throw OpError("internal", phase, QStringLiteral("неверные параметры передачи данных"));
    RunConfig result;
    result.phase = phase;
    result.duration = std::chrono::seconds(config.durationSeconds);
    result.warmup = warmup;
    result.startupTimeout = startupTimeout;
    result.readyGrace = readyGrace;
    // An explicit connection count is a user limit as well as an initial count.
    result.initialWorkers = config.connections == 0 ? std::clamp(nativeWorkers, 1, 16) : config.connections;
    result.maxWorkers = config.connections == 0 ? 16 : config.connections;
    if (config.connections == 0) result.adaptive = adaptiveConnections;
    return result;
}

RunResult run(const RunConfig& config, const Cancellation& cancellation,
              const Worker& worker, Progress report) {
    RunResult result;
    const auto phase = config.phase;
    auto invalid = [&](const QString& text) {
        result.error.emplace("internal", phase, text);
        return result;
    };
    if (!worker || config.duration.count() <= 0 || config.warmup.count() < 0 ||
        config.startupTimeout.count() <= 0 || config.readyGrace.count() < 0 ||
        config.reconnectDelay.count() < 0 || config.progressInterval.count() <= 0 ||
        config.initialWorkers < 1 || config.initialWorkers > 16 ||
        config.maxWorkers < config.initialWorkers || config.maxWorkers > 16 ||
        config.reconnects < 0 || config.reconnects > 1)
        return invalid(QStringLiteral("неверная конфигурация потоков передачи"));
    if (cancellation.canceled()) {
        result.error.emplace("canceled", phase, QStringLiteral("операция отменена"));
        return result;
    }

    Shared shared;
    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(config.maxWorkers));
    int workers = 0;
    TimePoint measurementStart{}, measurementFinish{};
    bool allEndedEarly = false;
    std::optional<OpError> failure;
    {
        JoinWorkers join{shared, threads};
        auto startWorker = [&](int index) {
            threads.emplace_back([&, index] {
                for (int attempt = 0; attempt <= config.reconnects && !stopped(shared, cancellation); ++attempt) {
                    bool attemptReady = false;
                    WorkerContext context;
                    context.stopped = [&] { return stopped(shared, cancellation); };
                    context.ready = [&] {
                        std::lock_guard lock(shared.mutex);
                        if (attemptReady || stopped(shared, cancellation)) return;
                        attemptReady = true;
                        ++shared.active;
                        auto& state = shared.states[static_cast<size_t>(index)];
                        if (!state.ready) { state.ready = true; ++shared.ready; }
                        shared.changed.notify_all();
                    };
                    context.record = [&](qint64 count) {
                        if (count <= 0) return;
                        std::lock_guard lock(shared.mutex);
                        if (!attemptReady || stopped(shared, cancellation)) return;
                        if (shared.measuring) {
                            auto& measured = shared.states[static_cast<size_t>(index)].measured;
                            // Check both counters before either is modified.
                            if (count > std::numeric_limits<qint64>::max() - shared.bytes ||
                                count > std::numeric_limits<qint64>::max() - measured)
                                throw OpError("protocol", phase, QStringLiteral("счётчик подтверждённых данных переполнен"));
                            shared.bytes += count;
                            measured += count;
                        } else addBytes(shared.warmupBytes, count);
                    };
                    std::optional<OpError> error;
                    try {
                        worker(index, attempt, context);
                        if (!context.stopped()) error.emplace("unavailable", phase, QStringLiteral("поток передачи завершился раньше времени"), true);
                    } catch (const OpError& value) { error = value; }
                      catch (...) { error.emplace("internal", phase, QStringLiteral("поток передачи аварийно завершился")); }
                    {
                        std::lock_guard lock(shared.mutex);
                        if (attemptReady) --shared.active;
                        if (error && !stopped(shared, cancellation))
                            shared.states[static_cast<size_t>(index)].errors.append(*error);
                    }
                    if (attempt < config.reconnects) {
                        std::unique_lock lock(shared.mutex);
                        const auto resume = Clock::now() + config.reconnectDelay;
                        while (!stopped(shared, cancellation) && Clock::now() < resume)
                            shared.changed.wait_for(lock, std::min(Milliseconds{10},
                                std::chrono::duration_cast<Milliseconds>(resume - Clock::now()) + Milliseconds{1}));
                    }
                }
                std::lock_guard lock(shared.mutex);
                ++shared.done;
                shared.changed.notify_all();
            });
            ++workers;
        };

        auto waitUntil = [&](TimePoint until, bool firstReady) {
            std::unique_lock lock(shared.mutex);
            for (;;) {
                if (cancellation.canceled()) {
                    failure.emplace("canceled", phase, QStringLiteral("операция отменена"));
                    return false;
                }
                if (shared.done >= workers) {
                    allEndedEarly = true;
                    failure.emplace("unavailable", phase, QStringLiteral("все потоки передачи остановились раньше времени"), true);
                    return false;
                }
                if (firstReady && shared.ready > 0) return true;
                if (Clock::now() >= until) {
                    if (firstReady) failure.emplace("timeout", phase, QStringLiteral("ни один поток передачи не готов за отведённое время"), true);
                    return !firstReady;
                }
                shared.changed.wait_until(lock, std::min(until, Clock::now() + Milliseconds{10}));
            }
        };

        try {
            for (int index = 0; index < config.initialWorkers; ++index) startWorker(index);
            bool ready = waitUntil(Clock::now() + config.startupTimeout, true);
            if (ready) ready = waitUntil(Clock::now() + config.readyGrace, false);
            if (ready && config.warmup.count() > 0) {
                const auto warmupStart = Clock::now();
                { std::lock_guard lock(shared.mutex); shared.warmupBytes = 0; }
                ready = waitUntil(warmupStart + config.warmup, false);
                if (ready && config.adaptive) {
                    qint64 warmupBytes;
                    { std::lock_guard lock(shared.mutex); warmupBytes = shared.warmupBytes; }
                    const int target = std::clamp(config.adaptive(mbps(warmupBytes, seconds(Clock::now() - warmupStart))), workers, config.maxWorkers);
                    const int previous = workers;
                    for (int index = workers; index < target; ++index) startWorker(index);
                    if (target > previous) ready = waitUntil(Clock::now() + config.readyGrace, false);
                }
            }
            if (ready) {
                TimePoint deadline;
                {
                    std::lock_guard lock(shared.mutex);
                    measurementStart = Clock::now();
                    deadline = measurementStart + config.duration;
                    shared.deadlineNanos.store(nanos(deadline));
                    shared.measuring = true;
                }
                auto nextReport = measurementStart + config.progressInterval;
                auto lastReport = measurementStart;
                qint64 lastBytes = 0;
                double smoothed = 0;
                bool hasSample = false;
                for (;;) {
                    qint64 bytes;
                    int active;
                    TimePoint now;
                    {
                        std::unique_lock lock(shared.mutex);
                        now = Clock::now();
                        if (cancellation.canceled()) {
                            failure.emplace("canceled", phase, QStringLiteral("операция отменена"));
                            break;
                        }
                        if (now >= deadline) break;
                        if (shared.done >= workers) {
                            allEndedEarly = true;
                            failure.emplace("unavailable", phase, QStringLiteral("все потоки передачи остановились раньше времени"), true);
                            break;
                        }
                        if (!report || now < nextReport) {
                            shared.changed.wait_until(lock, std::min({deadline, report ? nextReport : deadline, now + Milliseconds{10}}));
                            continue;
                        }
                        bytes = shared.bytes;
                        active = shared.active;
                    }
                    const double instant = mbps(bytes - lastBytes, seconds(now - lastReport));
                    smoothed = hasSample ? smoothed + .2 * (instant - smoothed) : instant;
                    hasSample = true;
                    report({{"mbps", smoothed}, {"bytes", bytes},
                            {"elapsed_ms", seconds(now - measurementStart) * 1000}, {"active", active}});
                    lastBytes = bytes;
                    lastReport = now;
                    nextReport = now + config.progressInterval;
                }
                measurementFinish = std::min(Clock::now(), deadline);
            }
        } catch (const OpError& error) { failure = error; }
          catch (...) { failure.emplace("internal", phase, QStringLiteral("не удалось выполнить передачу данных")); }
        if (measurementStart != TimePoint{} && measurementFinish == TimePoint{})
            measurementFinish = std::min(Clock::now(), measurementStart + config.duration);
        // Freeze accounting before the join; slow transport cleanup must not
        // lower the measured speed or extend the measurement window.
        shared.stop.store(true);
        std::lock_guard lock(shared.mutex);
        shared.measuring = false;
    }
    if (measurementStart != TimePoint{}) result.elapsedSeconds = seconds(measurementFinish - measurementStart);
    result.bytes = shared.bytes;
    for (int index = 0; index < workers; ++index) {
        const auto& state = shared.states[static_cast<size_t>(index)];
        if (state.measured > 0) ++result.workersOK; else ++result.workersFailed;
        result.workerErrors.append(state.errors);
    }
    if (cancellation.canceled()) failure.emplace("canceled", phase, QStringLiteral("операция отменена"));
    if (!failure && (result.bytes == 0 || result.workersOK == 0))
        failure.emplace("protocol", phase, QStringLiteral("передача завершилась без подтверждённых данных"));
    if (allEndedEarly && !result.workerErrors.isEmpty() && failure && failure->code != "canceled") {
        const auto& first = result.workerErrors.first();
        failure.emplace(first.code, phase,
                        QStringLiteral("все потоки передачи завершились с ошибкой: %1 (сбоев: %2)")
                            .arg(first.message).arg(result.workerErrors.size()), first.retryable);
    }
    result.error = failure;
    return result;
}

QJsonObject phaseResult(const RunResult& result, const QString& method) {
    // Schema 1 uses method for the ping statistic; throughput keeps it null.
    Q_UNUSED(method);
    auto phase = emptyPhase("ok");
    if (result.bytes > 0 || result.elapsedSeconds > 0 || result.workersOK > 0 || result.workersFailed > 0) {
        phase.insert("bytes", result.bytes);
        phase.insert("successful_streams", result.workersOK);
        phase.insert("failed_streams", result.workersFailed);
    }
    if (result.elapsedSeconds > 0 && std::isfinite(result.elapsedSeconds))
        phase.insert("elapsed_ms", std::round(result.elapsedSeconds * 100000) / 100);
    if (!result.workerErrors.isEmpty()) phase.insert("warnings", QJsonArray{
        QStringLiteral("сетевые сбои отдельных потоков: %1").arg(result.workerErrors.size())});
    if (result.error) {
        auto error = *result.error;
        error.transferDiagnostics = phase;
        throw error;
    }
    if (result.bytes <= 0 || result.workersOK <= 0 || result.elapsedSeconds <= 0 || !std::isfinite(result.elapsedSeconds))
        throw OpError("protocol", "transfer", QStringLiteral("передача завершилась без подтверждённых данных"));
    phase.insert("mbps", std::round(mbps(result.bytes, result.elapsedSeconds) * 100) / 100);
    return phase;
}

} // namespace puls::transfer
