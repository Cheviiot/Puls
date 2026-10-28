#pragma once

#include <QUrl>
#include <QString>
#include <QStringList>
#include <memory>
#include <chrono>
#include <optional>

namespace puls {

class Backend;

// Test-only endpoint injection. HTTP/WS is accepted only for loopback hosts
// when allowLoopbackHTTP is explicitly enabled. Production code uses
// makeBackend(), which has no endpoint override.
struct BackendOptions {
    QUrl yandexBase = QUrl(QStringLiteral("https://yandex.ru"));
    QUrl speedtestBase = QUrl(QStringLiteral("https://speedtest.ru"));
    bool allowLoopbackHTTP = false;
    // Test-only QMS fallback targets. Nullopt uses the production first-party list;
    // makeBackendForTesting replaces an unspecified value with an empty list.
    std::optional<QStringList> speedtestFallbackHosts;
    // Test-only timing injection; never exposed as a CLI flag or environment.
    std::optional<std::chrono::milliseconds> transferDuration;
    std::optional<std::chrono::milliseconds> transferWarmup;
    std::optional<std::chrono::milliseconds> transferReadyGrace;
    std::optional<std::chrono::milliseconds> transferStartupTimeout;
};

std::unique_ptr<Backend> makeBackendForTesting(const QString& service,
                                               const BackendOptions& options);

} // namespace puls
