#pragma once

#include "puls/transfer_network.hpp"
#include <QNetworkAccessManager>

namespace puls::yandex_transfer {

// Private worker seams used by loopback protocol tests. No endpoints or timing
// overrides are exposed through the public CLI.
bool downloadProbe(QNetworkAccessManager&, const QUrl&, const BackendOptions&,
                   const transfer::WorkerContext&);
void httpUpload(QNetworkAccessManager&, const QUrl&, const BackendOptions&,
                const transfer::WorkerContext&);
void websocketUpload(const YandexUploadProbe&, const BackendOptions&,
                     std::chrono::milliseconds sessionDuration,
                     const transfer::WorkerContext&);

} // namespace puls::yandex_transfer
