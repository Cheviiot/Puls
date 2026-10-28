#pragma once

#include "puls/network.hpp"
#include "puls/transfer.hpp"
#include <mutex>

namespace puls {

struct YandexUploadProbe {
    QUrl postUrl;
    QUrl websocketUrl;
    // Zero means the discovery response did not supply an override.
    int websocketConnectionTimeoutMs = 0;
};

QJsonObject yandexDownload(const Config&, const Cancellation&, const BackendOptions&,
                          const QList<QUrl>& urls, transfer::Progress = {});
QJsonObject yandexUpload(const Config&, const Cancellation&, const BackendOptions&,
                        const QList<YandexUploadProbe>& probes, transfer::Progress = {});

// Shared only during one backend invocation. Access credentials under mutex;
// create QNetworkAccessManager in each calling worker, never share it.
struct SpeedtestSession {
    std::mutex mutex;
    QString browserKey = QStringLiteral("5f3287b55fbcd8076919114885f8f3f7");
    bool keyRefreshAttempted = false;
    QString jwt;
};

QString refreshSpeedtestBrowserKey(SpeedtestSession&, const BackendOptions&,
                                  const Cancellation&, const QString& phase);
// servers are already ranked HTTPS base URLs (loopback HTTP in mocks only).
QJsonObject speedtestDownload(const Config&, const Cancellation&, const BackendOptions&,
                             const QList<QUrl>& servers, SpeedtestSession&,
                             transfer::Progress = {});
QJsonObject speedtestUpload(const Config&, const Cancellation&, const BackendOptions&,
                           const QList<QUrl>& servers, SpeedtestSession&,
                           transfer::Progress = {});

} // namespace puls
