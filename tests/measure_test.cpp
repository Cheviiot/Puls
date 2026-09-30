#include "puls/core/text.hpp"
#include "puls/measure/measure.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace puls::measure {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

Error wait_done(const Context& ctx) {
    ctx.wait();
    return ctx.err();
}

TEST(Mbps, ConvertsBytesAndDuration) {
    EXPECT_DOUBLE_EQ(mbps(0, 1s), 0);
    EXPECT_DOUBLE_EQ(mbps(1'000'000, 1s), 8);
    EXPECT_DOUBLE_EQ(mbps(12'500'000, 1s), 100);
    EXPECT_DOUBLE_EQ(mbps(6'250'000, 500ms), 100);
}

TEST(Mbps, GuardsZeroAndNegativeInputs) {
    EXPECT_EQ(mbps(1'000'000, 0s), 0);
    EXPECT_EQ(mbps(1'000'000, -1s), 0);
    EXPECT_EQ(mbps(-1, 1s), 0);
}

TEST(Ema, DampsSpikesAndConverges) {
    const double moved = ema(50, 500);
    EXPECT_GT(moved, 50);
    EXPECT_LT(moved, 50 + (500 - 50) * ema_alpha + 1e-9);
    double smoothed = 0;
    for (int index = 0; index < 100; ++index) {
        smoothed = ema(smoothed, 100);
    }
    EXPECT_NEAR(smoothed, 100, 0.01);
}

TEST(Run, StartsClockAfterReadyAndExcludesWarmup) {
    const auto started = Clock::now();
    RunConfig config;
    config.duration = 60ms;
    config.warmup = 70ms;
    config.startup_timeout = 1s;
    config.ready_grace = 5ms;
    config.initial_workers = 1;
    config.max_workers = 1;
    const auto outcome = run(
        Context(), config,
        [](const Context& ctx, int, const ReadyFn& ready, const RecordFn& record) {
            std::this_thread::sleep_for(50ms);
            ready();
            record(111);
            std::this_thread::sleep_for(40ms);
            record(222);
            std::this_thread::sleep_for(50ms);
            record(333);
            return wait_done(ctx);
        },
        nullptr);
    ASSERT_FALSE(outcome.error) << outcome.error.message();
    EXPECT_EQ(outcome.result.bytes, 333);
    EXPECT_GE(outcome.result.elapsed, 50ms);
    EXPECT_LE(outcome.result.elapsed, 130ms);
    EXPECT_GE(Clock::now() - started, 170ms);
}

TEST(Run, FailsEarlyWhenAllWorkersDie) {
    const auto started = Clock::now();
    RunConfig config;
    config.duration = 1s;
    config.startup_timeout = 2s;
    config.ready_grace = 5ms;
    config.initial_workers = 2;
    config.max_workers = 2;
    const auto outcome = run(
        Context(), config,
        [](const Context&, int, const ReadyFn&, const RecordFn&) {
            return Error::make("dial failed");
        },
        nullptr);
    EXPECT_TRUE(outcome.error);
    EXPECT_LT(Clock::now() - started, 300ms);
}

TEST(Run, ReconnectsOnceAndCountsConfirmedBytes) {
    std::atomic<int> attempts{0};
    RunConfig config;
    config.duration = 350ms;
    config.startup_timeout = 1s;
    config.ready_grace = 5ms;
    config.initial_workers = 1;
    config.max_workers = 1;
    config.reconnects = 1;
    const auto outcome = run(
        Context(), config,
        [&attempts](const Context& ctx, int, const ReadyFn& ready, const RecordFn& record) {
            const int attempt = attempts.fetch_add(1) + 1;
            ready();
            if (attempt == 1) {
                return Error::make("connection reset");
            }
            record(4096);
            return wait_done(ctx);
        },
        nullptr);
    ASSERT_FALSE(outcome.error) << outcome.error.message();
    EXPECT_EQ(attempts.load(), 2);
    EXPECT_EQ(outcome.result.bytes, 4096);
    EXPECT_EQ(outcome.result.workers_ok, 1);
    EXPECT_FALSE(outcome.result.worker_errors.empty());
}

TEST(Run, CancellationReturnsPromptly) {
    CancelScope scope{Context()};
    std::thread canceler([&scope] {
        std::this_thread::sleep_for(30ms);
        scope.cancel();
    });
    const auto started = Clock::now();
    RunConfig config;
    config.duration = 10s;
    config.startup_timeout = 1s;
    config.ready_grace = 5ms;
    config.initial_workers = 1;
    config.max_workers = 1;
    const auto outcome = run(
        scope.context(), config,
        [](const Context& ctx, int, const ReadyFn& ready, const RecordFn&) {
            ready();
            return wait_done(ctx);
        },
        nullptr);
    canceler.join();
    EXPECT_TRUE(outcome.error.is(errors::canceled_tag)) << outcome.error.message();
    EXPECT_LT(Clock::now() - started, 1s);
}

TEST(Run, ReturnsPartialStreamDiagnostics) {
    RunConfig config;
    config.duration = 80ms;
    config.startup_timeout = 1s;
    config.ready_grace = 5ms;
    config.initial_workers = 2;
    config.max_workers = 2;
    const auto outcome = run(
        Context(), config,
        [](const Context& ctx, int index, const ReadyFn& ready, const RecordFn& record) {
            ready();
            if (index == 1) {
                return Error::make("one stream failed");
            }
            std::this_thread::sleep_for(20ms);
            record(2048);
            return wait_done(ctx);
        },
        nullptr);
    ASSERT_FALSE(outcome.error) << outcome.error.message();
    EXPECT_EQ(outcome.result.bytes, 2048);
    EXPECT_EQ(outcome.result.workers_ok, 1);
    EXPECT_EQ(outcome.result.workers_failed, 1);
    EXPECT_EQ(outcome.result.worker_errors.size(), 1u);
}

TEST(Run, AggregatesStartupWorkerErrors) {
    const Error first = Error::make("first dial failed");
    const Error second = Error::make("second dial failed");
    RunConfig config;
    config.duration = 1s;
    config.startup_timeout = 1s;
    config.ready_grace = 1ms;
    config.initial_workers = 2;
    config.max_workers = 2;
    const auto outcome = run(
        Context(), config,
        [&](const Context&, int index, const ReadyFn&, const RecordFn&) {
            return index == 0 ? first : second;
        },
        nullptr);
    EXPECT_TRUE(outcome.error.is(first));
    EXPECT_TRUE(outcome.error.is(second));
    EXPECT_EQ(outcome.result.workers_failed, 2);
    EXPECT_EQ(outcome.result.workers_ok, 0);
    EXPECT_EQ(outcome.result.worker_errors.size(), 2u);
}

TEST(Run, RecoversWorkerException) {
    RunConfig config;
    config.duration = 1s;
    config.startup_timeout = 1s;
    config.ready_grace = 1ms;
    config.initial_workers = 1;
    config.max_workers = 1;
    const auto outcome = run(
        Context(), config,
        [](const Context&, int, const ReadyFn&, const RecordFn&) -> Error {
            throw std::runtime_error("broken worker");
        },
        nullptr);
    EXPECT_TRUE(text::contains(outcome.error.message(), "broken worker"))
        << outcome.error.message();
    EXPECT_EQ(outcome.result.workers_failed, 1);
    EXPECT_EQ(outcome.result.worker_errors.size(), 1u);
}

TEST(Run, ReadyIsIdempotentUnderConcurrentCalls) {
    std::atomic<int> maximum_active{0};
    RunConfig config;
    config.duration = 260ms;
    config.startup_timeout = 1s;
    config.ready_grace = 1ms;
    config.initial_workers = 1;
    config.max_workers = 1;
    const auto outcome = run(
        Context(), config,
        [](const Context& ctx, int, const ReadyFn& ready, const RecordFn& record) {
            std::vector<std::thread> callers;
            for (int index = 0; index < 32; ++index) {
                callers.emplace_back([&ready] { ready(); });
            }
            for (auto& caller : callers) {
                caller.join();
            }
            while (!ctx.wait_for(5ms)) {
                record(1024);
            }
            return ctx.err();
        },
        [&maximum_active](const RunProgress& progress) {
            int previous = maximum_active.load();
            while (progress.active > previous &&
                   !maximum_active.compare_exchange_weak(previous, progress.active)) {
            }
        });
    ASSERT_FALSE(outcome.error) << outcome.error.message();
    EXPECT_GT(outcome.result.bytes, 0);
    EXPECT_EQ(outcome.result.workers_ok, 1);
    EXPECT_EQ(maximum_active.load(), 1);
}

TEST(Run, CancellationInterruptsReconnectBackoff) {
    CancelScope scope{Context()};
    std::atomic<int> attempts{0};
    std::promise<void> first_attempt;
    auto first_attempt_done = first_attempt.get_future();
    RunConfig config;
    config.duration = 1s;
    config.startup_timeout = 1s;
    config.ready_grace = 1ms;
    config.initial_workers = 1;
    config.max_workers = 1;
    config.reconnects = 1;
    auto result = std::async(std::launch::async, [&] {
        return run(
            scope.context(), config,
            [&](const Context&, int, const ReadyFn&, const RecordFn&) {
                if (attempts.fetch_add(1) == 0) {
                    first_attempt.set_value();
                }
                return Error::make("connection reset");
            },
            nullptr);
    });
    first_attempt_done.wait();
    scope.cancel();
    ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(result.get().error.is(errors::canceled_tag));
    EXPECT_EQ(attempts.load(), 1);
}

TEST(Run, RejectsUnsafeConfigurationBeforeStartingWorker) {
    const auto config = [](int initial, int maximum, int reconnects) {
        RunConfig value;
        value.duration = 1s;
        value.initial_workers = initial;
        value.max_workers = maximum;
        value.reconnects = reconnects;
        return value;
    };
    for (const RunConfig& unsafe :
         {config(17, 17, 0), config(1, 17, 0), config(1, 1, -1), config(1, 1, 2)}) {
        bool called = false;
        const auto outcome = run(
            Context(), unsafe,
            [&called](const Context&, int, const ReadyFn&, const RecordFn&) {
                called = true;
                return Error();
            },
            nullptr);
        EXPECT_TRUE(outcome.error);
        EXPECT_FALSE(called);
    }
    RunConfig valid;
    valid.duration = 1s;
    EXPECT_TRUE(run(Context(), valid, nullptr, nullptr).error);
    RunConfig no_duration;
    EXPECT_TRUE(run(
                    Context(), no_duration,
                    [](const Context&, int, const ReadyFn&, const RecordFn&) { return Error(); },
                    nullptr)
                    .error);
}

TEST(Run, WaitsForEveryWorkerOnCancellation) {
    CancelScope scope{Context()};
    std::atomic<int> started{0};
    std::atomic<int> exited{0};
    std::promise<void> all_started;
    RunConfig config;
    config.duration = 10s;
    config.startup_timeout = 1s;
    config.ready_grace = 5ms;
    config.initial_workers = 16;
    config.max_workers = 16;
    auto result = std::async(std::launch::async, [&] {
        return run(
            scope.context(), config,
            [&](const Context& ctx, int, const ReadyFn& ready, const RecordFn&) {
                ready();
                if (started.fetch_add(1) + 1 == 16) {
                    all_started.set_value();
                }
                Error error = wait_done(ctx);
                exited.fetch_add(1);
                return error;
            },
            nullptr);
    });
    all_started.get_future().wait();
    scope.cancel();
    ASSERT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(result.get().error.is(errors::canceled_tag));
    EXPECT_EQ(exited.load(), 16);
}

// Workers that notice the caller's cancellation first exit with their own
// errors; the run still reports the cancellation.
TEST(Run, ReportsCancellationWhenWorkersExitFirst) {
    for (int iteration = 0; iteration < 50; ++iteration) {
        CancelScope scope{Context()};
        std::atomic<int> started{0};
        std::promise<void> all_started;
        RunConfig config;
        config.duration = 10s;
        config.startup_timeout = 5s;
        config.ready_grace = 5s;
        config.initial_workers = 4;
        config.max_workers = 4;
        auto result = std::async(std::launch::async, [&] {
            return run(
                scope.context(), config,
                [&](const Context&, int, const ReadyFn& ready, const RecordFn&) {
                    ready();
                    if (started.fetch_add(1) + 1 == 4) {
                        all_started.set_value();
                    }
                    scope.context().wait();
                    return Error::make("stream closed");
                },
                nullptr);
        });
        all_started.get_future().wait();
        scope.cancel();
        ASSERT_EQ(result.wait_for(2s), std::future_status::ready);
        const Error error = result.get().error;
        ASSERT_TRUE(error.is(errors::canceled_tag)) << error.message();
    }
}

TEST(Run, ContainsWorkerErrorCallbackException) {
    RunConfig config;
    config.duration = 1s;
    config.startup_timeout = 1s;
    config.ready_grace = 1ms;
    config.initial_workers = 1;
    config.max_workers = 1;
    config.on_worker_error = [](int, int, const Error&) {
        throw std::runtime_error("broken callback");
    };
    const auto outcome = run(
        Context(), config,
        [](const Context&, int, const ReadyFn&, const RecordFn&) {
            return Error::make("dial failed");
        },
        nullptr);
    EXPECT_TRUE(text::contains(outcome.error.message(), "broken callback"))
        << outcome.error.message();
    EXPECT_EQ(outcome.result.workers_failed, 1);
    EXPECT_EQ(outcome.result.worker_errors.size(), 2u);
}

TEST(Run, CountsConcurrentConfirmedBytesExactly) {
    constexpr int workers = 8;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
    std::once_flag release_once;
    RunConfig config;
    config.duration = 280ms;
    config.startup_timeout = 1s;
    config.ready_grace = 5ms;
    config.initial_workers = workers;
    config.max_workers = workers;
    const auto outcome = run(
        Context(), config,
        [released](const Context& ctx, int index, const ReadyFn& ready, const RecordFn& record) {
            ready();
            released.wait();
            record(-1);
            record(0);
            record(static_cast<std::int64_t>(index + 1) * 1000);
            return wait_done(ctx);
        },
        [&](const RunProgress&) {
            std::call_once(release_once, [&release] { release.set_value(); });
        });
    ASSERT_FALSE(outcome.error) << outcome.error.message();
    EXPECT_EQ(outcome.result.bytes, 36'000);
    EXPECT_EQ(outcome.result.workers_ok, workers);
    EXPECT_EQ(outcome.result.workers_failed, 0);
}

TEST(Run, AddsAdaptiveWorkersAfterWarmup) {
    std::atomic<int> started{0};
    RunConfig config;
    config.duration = 100ms;
    config.warmup = 30ms;
    config.startup_timeout = 1s;
    config.ready_grace = 5ms;
    config.initial_workers = 2;
    config.max_workers = 6;
    config.adaptive = [](double) { return 16; };
    const auto outcome = run(
        Context(), config,
        [&started](const Context& ctx, int, const ReadyFn& ready, const RecordFn& record) {
            started.fetch_add(1);
            ready();
            while (!ctx.wait_for(5ms)) {
                record(100);
            }
            return ctx.err();
        },
        nullptr);
    ASSERT_FALSE(outcome.error) << outcome.error.message();
    EXPECT_EQ(started.load(), 6);
    EXPECT_EQ(outcome.result.workers_ok, 6);
}

TEST(Run, ReportsEarlyEndOfAllStreams) {
    RunConfig config;
    config.duration = 2s;
    config.startup_timeout = 1s;
    config.ready_grace = 1ms;
    config.initial_workers = 1;
    config.max_workers = 1;
    const auto started = Clock::now();
    const auto outcome = run(
        Context(), config,
        [](const Context&, int, const ReadyFn& ready, const RecordFn& record) {
            ready();
            std::this_thread::sleep_for(20ms);
            record(10);
            return Error();
        },
        nullptr);
    EXPECT_EQ(outcome.error.message().rfind("все потоки передачи остановились раньше времени", 0),
              0u)
        << outcome.error.message();
    EXPECT_LT(Clock::now() - started, 1s);
}

TEST(Run, StartupTimeoutWhenNoWorkerBecomesReady) {
    RunConfig config;
    config.duration = 1s;
    config.startup_timeout = 50ms;
    config.initial_workers = 1;
    config.max_workers = 1;
    const auto outcome = run(
        Context(), config,
        [](const Context& ctx, int, const ReadyFn&, const RecordFn&) { return wait_done(ctx); },
        nullptr);
    EXPECT_EQ(outcome.error.message(), "ни один поток не был готов за 50ms");
}

} // namespace
} // namespace puls::measure
