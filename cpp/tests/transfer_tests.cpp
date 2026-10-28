#include "puls/transfer.hpp"

#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using puls::transfer::Milliseconds;
using puls::transfer::RunConfig;
using puls::transfer::RunResult;

int failures = 0;

void expect(bool condition, const char* label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

RunConfig config(Milliseconds duration = 50ms, int workers = 1) {
    RunConfig result;
    result.phase = "download";
    result.duration = duration;
    result.startupTimeout = 200ms;
    result.readyGrace = 2ms;
    result.reconnectDelay = 1ms;
    result.progressInterval = 5ms;
    result.initialWorkers = workers;
    result.maxWorkers = workers;
    return result;
}

void waitStopped(const puls::transfer::WorkerContext& context) {
    while (!context.stopped()) std::this_thread::sleep_for(1ms);
}

void testReadinessWarmupAndDeadlineAccounting() {
    auto runConfig = config(70ms);
    runConfig.warmup = 25ms;
    std::atomic_bool measuring{false};
    std::atomic_bool exited{false};
    const auto result = puls::transfer::run(runConfig, {}, [&](int, int, const auto& context) {
        context.record(9000); // A protocol callback before ready must be ignored.
        context.ready();
        context.record(1000); // Warmup bytes must not enter the measured result.
        while (!context.stopped() && !measuring.load()) std::this_thread::sleep_for(1ms);
        if (measuring.load()) context.record(321);
        waitStopped(context);
        context.record(777); // The immutable deadline rejects late bytes.
        exited.store(true);
    }, [&](const QJsonObject&) { measuring.store(true); });

    expect(!result.error, "ready worker completes a nonempty measurement");
    expect(result.bytes == 321, "bytes before ready, during warmup, and after deadline are excluded");
    expect(exited.load(), "worker is joined before run returns");
}

void testStartupTimeoutJoinsWorker() {
    auto runConfig = config();
    runConfig.startupTimeout = 35ms;
    std::atomic_bool exited{false};
    const auto start = std::chrono::steady_clock::now();
    const auto result = puls::transfer::run(runConfig, {}, [&](int, int, const auto& context) {
        waitStopped(context);
        exited.store(true);
    });
    const auto elapsed = std::chrono::steady_clock::now() - start;
    expect(result.error && result.error->code == "timeout", "no ready worker produces startup timeout");
    expect(exited.load(), "startup timeout joins worker");
    expect(elapsed < 500ms, "startup timeout returns promptly");
}

void testAllWorkersFailEvenAfterBytes() {
    auto runConfig = config(400ms, 2);
    runConfig.reconnects = 0;
    std::atomic_int ready{0};
    std::atomic_bool measuring{false};
    const auto result = puls::transfer::run(runConfig, {}, [&](int, int, const auto& context) {
        context.ready();
        ++ready;
        while (!context.stopped() && !measuring.load()) std::this_thread::sleep_for(1ms);
        if (measuring.load()) context.record(10);
        throw puls::OpError("unavailable", "download", "mock worker failure", true);
    }, [&](const QJsonObject&) { measuring.store(true); });
    expect(ready.load() == 2, "both failing workers reached protocol readiness");
    expect(result.bytes > 0, "failed workers may have confirmed bytes before failing");
    expect(result.error.has_value(), "all workers failing early fails the phase despite bytes");
    expect(result.error && result.error->code == "unavailable", "all-worker failure retains network classification");
    expect(result.workerErrors.size() == 2, "all worker failures are aggregated");
}

void testPartialWorkerSuccessAndAggregatedError() {
    auto runConfig = config(60ms, 2);
    runConfig.reconnects = 0;
    std::atomic_bool measuring{false};
    const auto result = puls::transfer::run(runConfig, {}, [&](int index, int, const auto& context) {
        context.ready();
        if (index == 1) {
            while (!measuring.load() && !context.stopped()) std::this_thread::sleep_for(1ms);
            throw puls::OpError("unavailable", "download", "one stream failed", true);
        }
        while (!context.stopped()) {
            context.record(8);
            std::this_thread::sleep_for(2ms);
        }
    }, [&](const QJsonObject&) { measuring.store(true); });
    expect(!result.error, "one productive worker permits partial success");
    expect(result.bytes > 0 && result.workersOK == 1 && result.workersFailed == 1,
           "partial success reports confirmed bytes and worker counts");
    expect(result.workerErrors.size() == 1, "partial success retains failed worker diagnostic");
    const auto phase = puls::transfer::phaseResult(result, "mock");
    expect(phase.value("status").toString() == "ok" && phase.value("failed_streams").toInt() == 1,
           "phase result exposes partial worker failure");
    expect(phase.value("warnings").toArray().size() == 1, "partial worker error becomes a warning");
}

void testOnlyOneReconnect() {
    auto runConfig = config(40ms);
    runConfig.reconnects = 1;
    std::atomic_int attempts{0};
    const auto result = puls::transfer::run(runConfig, {}, [&](int, int attempt, const auto& context) {
        ++attempts;
        if (attempt == 0) {
            context.ready();
            throw puls::OpError("unavailable", "download", "first attempt disconnected", true);
        }
        context.ready();
        while (!context.stopped()) {
            context.record(9);
            std::this_thread::sleep_for(2ms);
        }
    });
    expect(attempts.load() == 2, "worker receives at most one reconnect attempt");
    expect(!result.error && result.bytes > 0, "reconnected worker completes the phase");
    expect(result.workerErrors.size() == 1, "reconnect diagnostic is retained");
}

void testConfigurationAndAdaptiveCap() {
    expect(puls::transfer::adaptiveConnections(0) == 8, "adaptive workers have minimum native default");
    expect(puls::transfer::adaptiveConnections(50) == 8, "adaptive threshold includes 50 Mbps");
    expect(puls::transfer::adaptiveConnections(50.1) == 12, "adaptive threshold increases above 50 Mbps");
    expect(puls::transfer::adaptiveConnections(300) == 12, "adaptive threshold includes 300 Mbps");
    expect(puls::transfer::adaptiveConnections(300.1) == 16, "adaptive worker count caps at 16");

    puls::Config native;
    native.durationSeconds = 3;
    native.connections = 0;
    const auto automatic = puls::transfer::configure(native, "download", 40, 10ms, 100ms, 5ms);
    expect(automatic.initialWorkers == 16 && automatic.maxWorkers == 16,
           "automatic workers are capped to the native maximum");
    native.connections = 4;
    const auto explicitCount = puls::transfer::configure(native, "download", 10, 0ms, 100ms, 5ms);
    expect(explicitCount.initialWorkers == 4 && explicitCount.maxWorkers == 4 && !explicitCount.adaptive,
           "explicit worker count is a fixed limit");

    auto invalid = config();
    invalid.maxWorkers = 17;
    std::atomic_int invoked{0};
    const auto failed = puls::transfer::run(invalid, {}, [&](int, int, const auto&) { ++invoked; });
    expect(failed.error && failed.error->code == "internal", "invalid runner configuration is returned as internal error");
    expect(invoked.load() == 0, "invalid configuration starts no workers");

    auto capped = config(35ms);
    capped.warmup = 5ms;
    capped.maxWorkers = 16;
    capped.adaptive = [](double) { return 100; };
    std::atomic_int starts{0};
    const auto capResult = puls::transfer::run(capped, {}, [&](int, int, const auto& context) {
        ++starts;
        context.ready();
        while (!context.stopped()) {
            context.record(1);
            std::this_thread::sleep_for(1ms);
        }
    }, {});
    expect(!capResult.error && starts.load() == 16, "adaptive callback cannot exceed 16 workers");
}

void testZeroBytesIsAnError() {
    const auto result = puls::transfer::run(config(35ms), {}, [](int, int, const auto& context) {
        context.ready();
        waitStopped(context);
    });
    expect(result.error && result.error->code == "protocol", "zero confirmed bytes cannot be successful");
    try {
        (void)puls::transfer::phaseResult(result, "mock");
        expect(false, "phase result rejects zero-byte run");
    } catch (const puls::OpError&) {
    }
}

void testCancellationAtStartupWarmupAndMeasurement() {
    auto runCanceled = [](RunConfig runConfig, bool readyImmediately) {
        puls::Cancellation cancellation;
        std::atomic_bool exited{false};
        RunResult result;
        std::thread runner([&] {
            result = puls::transfer::run(runConfig, cancellation, [&](int, int, const auto& context) {
                if (readyImmediately) context.ready();
                while (!context.stopped()) std::this_thread::sleep_for(1ms);
                exited.store(true);
            });
        });
        std::this_thread::sleep_for(15ms);
        const auto cancelAt = std::chrono::steady_clock::now();
        cancellation.cancel();
        runner.join();
        const auto cancelLatency = std::chrono::steady_clock::now() - cancelAt;
        expect(exited.load(), "canceled run joins its worker");
        expect(result.error && result.error->code == "canceled", "cancellation is reported with canceled status");
        expect(cancelLatency < 1s, "cancellation completes within one second");
    };

    auto startup = config(200ms);
    startup.startupTimeout = 500ms;
    runCanceled(startup, false);

    auto warmup = config(200ms);
    warmup.warmup = 500ms;
    runCanceled(warmup, true);

    auto measurement = config(500ms);
    measurement.progressInterval = 5ms;
    runCanceled(measurement, true);
}

void testProgressRunsOnCoordinatorAndElapsedExcludesTeardown() {
    auto runConfig = config(55ms);
    std::atomic_bool exited{false};
    const auto caller = std::this_thread::get_id();
    std::atomic_bool reporterOnCaller{true};
    const auto wallStart = std::chrono::steady_clock::now();
    const auto result = puls::transfer::run(runConfig, {}, [&](int, int, const auto& context) {
        context.ready();
        while (!context.stopped()) {
            context.record(16);
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(100ms); // Slow teardown must not extend elapsed accounting.
        exited.store(true);
    }, [&](const QJsonObject&) {
        if (std::this_thread::get_id() != caller) reporterOnCaller.store(false);
    });
    const auto wallElapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
    expect(!result.error && result.bytes > 0, "measurement has confirmed bytes");
    expect(reporterOnCaller.load(), "progress callback runs on coordinator thread");
    expect(exited.load(), "slow worker teardown completes before return");
    expect(result.elapsedSeconds >= 0.04 && result.elapsedSeconds <= 0.12,
           "measured elapsed time tracks the phase deadline");
    expect(wallElapsed - result.elapsedSeconds > 0.06,
           "worker teardown is excluded from measured elapsed time");
}

void testLateCoordinatorCannotAdmitAfterDeadlineBytes() {
    auto runConfig = config(40ms);
    runConfig.progressInterval = 5ms;
    std::atomic_bool lateRecordAttempted{false};
    const auto result = puls::transfer::run(runConfig, {}, [&](int, int, const auto& context) {
        context.ready();
        while (!context.stopped()) {
            context.record(1);
            std::this_thread::sleep_for(1ms);
        }
        context.record(1000000);
        lateRecordAttempted.store(true);
    }, [](const QJsonObject&) {
        std::this_thread::sleep_for(90ms); // Delay the coordinator beyond the measurement deadline.
    });
    expect(!result.error && result.bytes > 0, "late coordinator still returns pre-deadline measurement");
    expect(lateRecordAttempted.load(), "worker attempts a confirmed-byte record after deadline");
    expect(result.bytes < 1000000, "post-deadline bytes are rejected while coordinator is blocked");
    expect(result.elapsedSeconds < 0.1, "coordinator delay does not extend measured elapsed time");
}

void testCallbackExceptionsJoinWorkers() {
    auto runConfig = config(500ms);
    std::atomic_bool exited{false};
    const auto result = puls::transfer::run(runConfig, {}, [&](int, int, const auto& context) {
        context.ready();
        while (!context.stopped()) {
            context.record(1);
            std::this_thread::sleep_for(1ms);
        }
        exited.store(true);
    }, [](const QJsonObject&) { throw std::runtime_error("observer failed"); });
    expect(result.error && result.error->code == "internal", "progress callback exception becomes internal error");
    expect(exited.load(), "callback exception stops and joins worker before return");

    auto adaptive = config(300ms);
    adaptive.warmup = 5ms;
    adaptive.adaptive = [](double) -> int { throw std::runtime_error("adaptive failed"); };
    exited.store(false);
    const auto adaptiveResult = puls::transfer::run(adaptive, {}, [&](int, int, const auto& context) {
        context.ready();
        while (!context.stopped()) std::this_thread::sleep_for(1ms);
        exited.store(true);
    });
    expect(adaptiveResult.error && adaptiveResult.error->code == "internal",
           "adaptive callback exception becomes internal error");
    expect(exited.load(), "adaptive callback exception joins worker before return");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testReadinessWarmupAndDeadlineAccounting();
    testStartupTimeoutJoinsWorker();
    testAllWorkersFailEvenAfterBytes();
    testPartialWorkerSuccessAndAggregatedError();
    testOnlyOneReconnect();
    testConfigurationAndAdaptiveCap();
    testZeroBytesIsAnError();
    testCancellationAtStartupWarmupAndMeasurement();
    testProgressRunsOnCoordinatorAndElapsedExcludesTeardown();
    testLateCoordinatorCannotAdmitAfterDeadlineBytes();
    testCallbackExceptionsJoinWorkers();
    if (failures) std::cerr << failures << " transfer test(s) failed\n";
    return failures == 0 ? 0 : 1;
}
