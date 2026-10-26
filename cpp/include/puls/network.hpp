#pragma once

#include <QUrl>
#include <QString>
#include <memory>

namespace puls {

class Backend;

// Test-only endpoint injection. HTTP/WS is accepted only for loopback hosts
// when allowLoopbackHTTP is explicitly enabled. Production code uses
// makeBackend(), which has no endpoint override.
struct BackendOptions {
    QUrl yandexBase = QUrl(QStringLiteral("https://yandex.ru"));
    QUrl speedtestBase = QUrl(QStringLiteral("https://speedtest.ru"));
    bool allowLoopbackHTTP = false;
};

std::unique_ptr<Backend> makeBackendForTesting(const QString& service,
                                               const BackendOptions& options);

} // namespace puls
