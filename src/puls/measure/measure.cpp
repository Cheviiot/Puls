#include "puls/measure/measure.hpp"

#include "puls/core/text.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace puls::measure {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

double seconds(Duration duration) {
    const auto whole = duration.count() / 1'000'000'000;
    const auto fraction = duration.count() % 1'000'000'000;
    return static_cast<double>(whole) + static_cast<double>(fraction) / 1e9;
}

struct WorkerState {
    std::atomic<std::int64_t> measured{0};
    std::atomic<bool> ready{false};
    // Written only by the worker thread and read after it has been joined.
    std::vector<Error> errors;
};

// Signals replace the buffered ready/done channels of the original design:
// workers post tokens and the coordinating thread consumes them.
class Signals {
public:
    enum class Event { ready, done, timeout, canceled };

    void post_ready() { post(ready_); }
    void post_done() { post(done_); }

    void cancel() {
        {
            std::lock_guard lock(mutex_);
            canceled_ = true;
        }
        changed_.notify_all();
    }

    Event wait(Clock::time_point until, bool accept_ready) {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (accept_ready && ready_ > 0) {
                --ready_;
                return Event::ready;
            }
            // Cancellation wins over worker exits it caused, so that callers
            // report the cancellation rather than a failure of every worker.
            if (canceled_) {
                return Event::canceled;
            }
            if (done_ > 0) {
                --done_;
                return Event::done;
            }
            if (Clock::now() >= until) {
                return Event::timeout;
            }
            changed_.wait_until(lock, until);
        }
    }

private:
    void post(int& counter) {
        {
            std::lock_guard lock(mutex_);
            ++counter;
        }
        changed_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    int ready_ = 0;
    int done_ = 0;
    bool canceled_ = false;
};

// Joins every started worker thread when the run finishes on any path.
class ThreadGroup {
public:
    explicit ThreadGroup(CancelScope& scope) : scope_(scope) {}
    ThreadGroup(const ThreadGroup&) = delete;
    ThreadGroup& operator=(const ThreadGroup&) = delete;
    ~ThreadGroup() { stop(); }

    template <class Function>
    void start(Function&& function) {
        threads_.emplace_back(std::forward<Function>(function));
    }

    void stop() {
        scope_.cancel();
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    CancelScope& scope_;
    std::vector<std::thread> threads_;
};

std::string exception_text(const std::exception_ptr& exception) {
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "неизвестное исключение";
    }
}

Error call_worker(const Worker& worker, const Context& ctx, int index, const ReadyFn& ready,
                  const RecordFn& record) {
    try {
        return worker(ctx, index, ready, record);
    } catch (...) {
        return Error::make("поток " + std::to_string(index + 1) +
                           " аварийно завершился: " + exception_text(std::current_exception()));
    }
}

Error call_worker_error(const RunConfig& config, int index, int attempt, const Error& error) {
    try {
        config.on_worker_error(index, attempt, error);
        return {};
    } catch (...) {
        return Error::make("обработчик ошибки потока " + std::to_string(index + 1) +
                           " аварийно завершился: " + exception_text(std::current_exception()));
    }
}

RunResult make_result(const std::unique_ptr<WorkerState[]>& states, int workers, std::int64_t bytes,
                      Duration elapsed) {
    RunResult result;
    result.bytes = bytes;
    result.elapsed = elapsed;
    for (int index = 0; index < workers; ++index) {
        const WorkerState& state = states[static_cast<std::size_t>(index)];
        if (state.measured.load() > 0) {
            ++result.workers_ok;
        } else {
            ++result.workers_failed;
        }
        result.worker_errors.insert(result.worker_errors.end(), state.errors.begin(),
                                    state.errors.end());
    }
    return result;
}

Error add_worker_errors(Error error, const std::vector<Error>& worker_errors) {
    if (worker_errors.empty()) {
        return error;
    }
    return Error::chain(std::move(error), Error::join(worker_errors));
}

} // namespace

double mbps(std::int64_t bytes, Duration elapsed) noexcept {
    if (bytes <= 0 || elapsed <= Duration::zero()) {
        return 0;
    }
    return static_cast<double>(bytes) * 8 / 1e6 / seconds(elapsed);
}

double ema(double previous, double instant) noexcept {
    return ema_alpha * instant + (1 - ema_alpha) * previous;
}

RunOutcome run(const Context& ctx, RunConfig config, const Worker& worker,
               const ProgressFn& report) {
    if (config.duration <= Duration::zero()) {
        return {{}, Error::make("длительность проверки должна быть положительной")};
    }
    if (!worker) {
        return {{}, Error::make("обработчик потока не задан")};
    }
    config.initial_workers = std::max(config.initial_workers, 1);
    if (config.initial_workers > max_run_workers) {
        return {{},
                Error::make("начальное число потоков (" + std::to_string(config.initial_workers) +
                            ") превышает предел " + std::to_string(max_run_workers))};
    }
    config.max_workers = std::max(config.max_workers, config.initial_workers);
    if (config.max_workers > max_run_workers) {
        return {{},
                Error::make("максимальное число потоков (" + std::to_string(config.max_workers) +
                            ") превышает предел " + std::to_string(max_run_workers))};
    }
    if (config.reconnects < 0 || config.reconnects > 1) {
        return {{},
                Error::make("число переподключений должно быть 0 или 1, получено " +
                            std::to_string(config.reconnects))};
    }
    if (config.startup_timeout <= Duration::zero()) {
        config.startup_timeout = 5s;
    }
    if (config.ready_grace <= Duration::zero()) {
        config.ready_grace = 300ms;
    }

    CancelScope run_scope(ctx);
    const Context& run_ctx = run_scope.context();

    std::atomic<std::int64_t> measured{0};
    std::atomic<std::int64_t> warmup{0};
    std::atomic<int> active{0};
    std::atomic<bool> measuring{false};
    Signals signals;
    auto states = std::make_unique<WorkerState[]>(static_cast<std::size_t>(config.max_workers));
    const ContextCallback cancellation = run_ctx.on_done([&signals] { signals.cancel(); });
    // Declared last so that worker threads are joined before the state they
    // reference is destroyed.
    ThreadGroup group(run_scope);

    const auto start_worker = [&](int index) {
        group.start([&, index] {
            WorkerState& state = states[static_cast<std::size_t>(index)];
            for (int attempt = 0; attempt <= config.reconnects; ++attempt) {
                if (run_ctx.done()) {
                    break;
                }
                std::atomic<bool> attempt_ready{false};
                const ReadyFn ready = [&] {
                    bool expected = false;
                    if (!attempt_ready.compare_exchange_strong(expected, true)) {
                        return;
                    }
                    active.fetch_add(1);
                    bool first = false;
                    if (state.ready.compare_exchange_strong(first, true)) {
                        signals.post_ready();
                    }
                };
                const RecordFn record = [&](std::int64_t bytes) {
                    if (bytes <= 0) {
                        return;
                    }
                    if (measuring.load()) {
                        measured.fetch_add(bytes);
                        state.measured.fetch_add(bytes);
                    } else {
                        warmup.fetch_add(bytes);
                    }
                };

                Error error = call_worker(worker, run_ctx, index, ready, record);
                if (attempt_ready.load()) {
                    active.fetch_sub(1);
                }
                if (run_ctx.done()) {
                    break;
                }
                if (!error) {
                    error = Error::make("поток " + std::to_string(index + 1) +
                                        " завершился раньше времени");
                }
                state.errors.push_back(error);
                if (config.on_worker_error) {
                    if (Error callback_error = call_worker_error(config, index, attempt, error)) {
                        state.errors.push_back(std::move(callback_error));
                    }
                }
                if (attempt < config.reconnects && run_ctx.wait_for(200ms * (attempt + 1))) {
                    break;
                }
            }
            signals.post_done();
        });
    };

    int workers = config.initial_workers;
    for (int index = 0; index < workers; ++index) {
        start_worker(index);
    }

    int done_workers = 0;
    const auto fail = [&](Error error) {
        // Workers can observe a cancellation and exit before its signal
        // reaches this thread; report the cancellation, not the exits.
        if (Error context_error = ctx.err()) {
            error = std::move(context_error);
        }
        group.stop();
        RunResult result = make_result(states, workers, measured.load(), Duration::zero());
        Error combined = add_worker_errors(std::move(error), result.worker_errors);
        return RunOutcome{std::move(result), std::move(combined)};
    };

    {
        const auto until = Clock::now() + config.startup_timeout;
        bool ready = false;
        while (!ready) {
            switch (signals.wait(until, true)) {
            case Signals::Event::ready:
                ready = true;
                break;
            case Signals::Event::done:
                if (++done_workers == workers) {
                    return fail(Error::make("все потоки (" + std::to_string(workers) +
                                            ") завершились с ошибкой при запуске"));
                }
                break;
            case Signals::Event::timeout:
                return fail(Error::make("ни один поток не был готов за " +
                                        text::format_duration(config.startup_timeout)));
            case Signals::Event::canceled:
                return fail(run_ctx.err());
            }
        }
    }

    const auto wait_stage = [&](Duration duration, std::string_view stage) -> Error {
        if (duration <= Duration::zero()) {
            return {};
        }
        const auto until = Clock::now() + duration;
        for (;;) {
            switch (signals.wait(until, false)) {
            case Signals::Event::timeout:
                return {};
            case Signals::Event::done:
                if (++done_workers >= workers) {
                    return Error::make("все потоки (" + std::to_string(workers) +
                                       ") завершились с ошибкой на этапе «" + std::string(stage) +
                                       "»");
                }
                break;
            case Signals::Event::canceled:
                return run_ctx.err();
            case Signals::Event::ready:
                break;
            }
        }
    };

    // Give the remaining initial workers a short opportunity to join without
    // making one slow endpoint hold the whole test hostage.
    if (Error error = wait_stage(config.ready_grace, "запуск")) {
        return fail(std::move(error));
    }

    if (config.warmup > Duration::zero()) {
        const auto warmup_start = Clock::now();
        if (Error error = wait_stage(config.warmup, "разогрев")) {
            return fail(std::move(error));
        }
        if (config.adaptive) {
            int target = config.adaptive(mbps(warmup.load(), Clock::now() - warmup_start));
            target = std::clamp(target, workers, config.max_workers);
            for (int index = workers; index < target; ++index) {
                start_worker(index);
            }
            workers = target;
            if (target > config.initial_workers) {
                if (Error error = wait_stage(config.ready_grace, "запуск дополнительных потоков")) {
                    return fail(std::move(error));
                }
            }
        }
    }

    const auto start = Clock::now();
    const auto deadline = start + config.duration;
    measuring.store(true);
    auto next_tick = start + 200ms;
    std::int64_t last_bytes = 0;
    auto last_time = start;
    double smoothed = 0;
    bool has_sample = false;
    bool ended_early = false;
    for (bool finished = false; !finished;) {
        const auto until = report ? std::min(deadline, next_tick) : deadline;
        switch (signals.wait(until, false)) {
        case Signals::Event::timeout: {
            const auto now = Clock::now();
            if (now >= deadline) {
                finished = true;
                break;
            }
            if (report && now >= next_tick) {
                const std::int64_t bytes = measured.load();
                const double instant = mbps(bytes - last_bytes, now - last_time);
                smoothed = has_sample ? ema(smoothed, instant) : instant;
                has_sample = true;
                report(RunProgress{smoothed, bytes, now - start, active.load()});
                last_bytes = bytes;
                last_time = now;
                next_tick += 200ms;
                if (next_tick <= Clock::now()) {
                    next_tick = Clock::now() + 200ms;
                }
            }
            break;
        }
        case Signals::Event::done:
            if (++done_workers >= workers) {
                ended_early = Clock::now() < deadline;
                finished = true;
            }
            break;
        case Signals::Event::canceled:
            finished = true;
            break;
        case Signals::Event::ready:
            break;
        }
    }
    measuring.store(false);
    const auto finish_at = Clock::now();
    group.stop();

    RunResult result = make_result(states, workers, measured.load(), finish_at - start);
    if (Error error = ctx.err()) {
        return {std::move(result), std::move(error)};
    }
    if (ended_early) {
        Error error = add_worker_errors(
            Error::make("все потоки передачи остановились раньше времени"), result.worker_errors);
        return {std::move(result), std::move(error)};
    }
    if (result.bytes == 0 || result.workers_ok == 0) {
        Error error = add_worker_errors(
            Error::make("передача завершилась без подтверждённых данных"), result.worker_errors);
        return {std::move(result), std::move(error)};
    }
    return {std::move(result), {}};
}

} // namespace puls::measure
