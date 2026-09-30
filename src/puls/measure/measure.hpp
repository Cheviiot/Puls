#pragma once

#include "puls/core/context.hpp"
#include "puls/core/error.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

// Throughput-measurement engine shared by every download/upload-capable
// service: bounded workers, warm-up, confirmed-byte accounting, one reconnect
// per worker and prompt cancellation.
namespace puls::measure {

using Duration = std::chrono::nanoseconds;

// Converts a byte count over a duration into megabits per second.
double mbps(std::int64_t bytes, Duration elapsed) noexcept;

// Exponential smoothing used for live progress. Raw per-tick throughput is
// bursty; smoothing keeps the displayed value readable while catching up with
// a genuine change within about a second. It never affects final results,
// which are always total bytes over the whole measured interval.
inline constexpr double ema_alpha = 0.2;
double ema(double previous, double instant) noexcept;

inline constexpr int max_run_workers = 16;

struct RunConfig {
    Duration duration{};
    Duration warmup{};
    Duration startup_timeout{};
    Duration ready_grace{};
    int initial_workers = 0;
    int max_workers = 0;
    int reconnects = 0;
    // Chooses the final worker count from the warm-up throughput.
    std::function<int(double warmup_mbps)> adaptive;
    // Diagnostics for a failed worker attempt.
    std::function<void(int index, int attempt, const Error& error)> on_worker_error;
};

struct RunProgress {
    double mbps = 0;
    std::int64_t bytes = 0;
    Duration elapsed{};
    int active = 0;
};

// Summary of a transfer. Worker errors are non-fatal when at least one worker
// transferred confirmed data.
struct RunResult {
    std::int64_t bytes = 0;
    Duration elapsed{};
    int workers_ok = 0;
    int workers_failed = 0;
    std::vector<Error> worker_errors;
};

struct RunOutcome {
    RunResult result;
    Error error;
};

using ReadyFn = std::function<void()>;
using RecordFn = std::function<void(std::int64_t)>;

// A worker must call ready once its connection and protocol handshake are
// usable, and record only bytes confirmed by the remote protocol. It returns
// when ctx is done or on failure. ready and record are thread-safe.
using Worker = std::function<Error(const Context& ctx, int index, const ReadyFn& ready,
                                   const RecordFn& record)>;

using ProgressFn = std::function<void(const RunProgress&)>;

// Starts a bounded group of workers, waits for at least one to become ready,
// performs an optional warm-up and only then starts the measurement clock. It
// fails early when every worker exits, and returns only after every worker
// thread has finished.
RunOutcome run(const Context& ctx, RunConfig config, const Worker& worker,
               const ProgressFn& report);

} // namespace puls::measure
