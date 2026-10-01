#pragma once

#include "puls/service/service.hpp"

#include <atomic>
#include <chrono>

namespace puls::testing {

// A measurement service with fixed results for frontend tests: the server
// mock.example in Владивосток, ping 11 ms, download 100 Mbit/s and upload
// 50 Mbit/s. Each operation can fail with a configured error.
class FakeBackend final : public service::Backend, public service::ConnectionInfoBackend {
public:
    explicit FakeBackend(service::ServiceId service) : service_(service) {}

    [[nodiscard]] service::ServiceId id() const override { return service_; }
    [[nodiscard]] service::Capability capabilities() const override {
        return service::Capability::ping | service::Capability::download |
               service::Capability::upload;
    }
    Result<service::Server> select_server(const Context&) override {
        ++select_calls;
        if (select_error) {
            return select_error;
        }
        return service::Server{"mock.example", "Владивосток", {}};
    }
    Result<service::PingResult> ping(const Context&) override {
        if (ping_error) {
            return ping_error;
        }
        return service::stats_with_method({10, 12, 11}, "median");
    }
    service::ThroughputOutcome download(const Context& ctx, const service::MeasurementConfig&,
                                        const service::ProgressFn& progress) override {
        using namespace std::chrono_literals;
        if (progress) {
            progress(service::ThroughputProgress{80, 1000, 500ms, 2});
        }
        if (block_download) {
            // Holds the transfer open until the measurement is canceled.
            ctx.wait();
            return {{}, ctx.err()};
        }
        service::ThroughputResult result{100, 12'500'000, 1s, 2, 0, {}};
        return {result, download_error};
    }
    service::ThroughputOutcome upload(const Context&, const service::MeasurementConfig&,
                                      const service::ProgressFn&) override {
        using namespace std::chrono_literals;
        service::ThroughputResult result{50, 6'250'000, 1s, 1, 0, {}};
        return {result, upload_error};
    }
    Result<service::ConnectionInfo> detect_connection(const Context&) override {
        ++connection_calls;
        if (connection_error) {
            return connection_error;
        }
        return connection;
    }

    Error select_error;
    Error ping_error;
    Error download_error;
    Error upload_error;
    std::atomic<bool> block_download{false};
    service::ConnectionInfo connection;
    Error connection_error;
    std::atomic<int> connection_calls{0};
    std::atomic<int> select_calls{0};

private:
    service::ServiceId service_;
};

} // namespace puls::testing
