#pragma once

#include "puls/application/types.hpp"
#include "puls/core/context.hpp"
#include "puls/net/tls.hpp"
#include "puls/service/service.hpp"

#include <functional>
#include <memory>
#include <string>

namespace puls::app {

// Creates a backend; server is the explicit speedtest.ru server or empty.
using BackendFactory = std::function<std::shared_ptr<service::Backend>(
    const std::string& server, const service::LogFunc& log)>;

struct RunnerOptions {
    // Null factories create the real Yandex and speedtest.ru backends.
    BackendFactory yandex_factory;
    BackendFactory speedtest_factory;
    service::LogFunc log;
    // TLS configuration for the default backends; null uses the system store.
    std::shared_ptr<net::TlsContext> tls;
};

// Validates a request before it is started from a user interface.
Error validate_measure_request(const MeasureRequest& request);

// Runner performs measurements and connection lookups for every frontend. It
// owns orchestration, retries and result conversion; frontends only render.
class Runner {
public:
    explicit Runner(RunnerOptions options = {});

    // Measures the requested services sequentially. A failure of one service
    // does not stop the others.
    Envelope measure(const Context& ctx, MeasureRequest request, const Observer& observer) const;
    // Detects the external IP address and Internet service provider.
    ConnectionResult detect_connection(const Context& ctx, const ConnectionRequest& request,
                                       const Observer& observer) const;

private:
    MeasurementResult measure_service(const Context& ctx, service::Backend& backend,
                                      const MeasureRequest& request,
                                      const Observer& observer) const;
    void run_throughput(const Context& ctx, service::Backend& backend,
                        const MeasureRequest& request, service::Phase phase,
                        MeasurementResult& result, const Observer& observer) const;
    [[nodiscard]] std::shared_ptr<service::Backend> backend(service::ServiceId id,
                                                            const std::string& server) const;
    void log(std::string_view message) const;

    RunnerOptions options_;
};

// Runs operation up to attempts times while its error is retryable, waiting
// 300 ms, 600 ms, ... between attempts.
Error with_retry(const Context& ctx, int attempts, const std::function<Error()>& operation);

} // namespace puls::app
