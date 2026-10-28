#include "yandex_throughput_p.hpp"

#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QWebSocket>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <optional>

namespace puls {
namespace {

constexpr auto userAgent = "Mozilla/5.0 (compatible; Puls; +https://github.com/Cheviiot/Puls)";
constexpr qint64 chunkSize = 64 << 10;
constexpr qint64 uploadSize = 2 << 20;
constexpr qint64 maxUploadResponse = 1 << 20;
constexpr qint64 maxDownloadSize = 1LL << 30;
constexpr qint64 nativeDownloadSize = 50 << 20;
constexpr qint64 maxAckSize = 64 << 10;
constexpr qint64 maxBufferedBytes = 1 << 20;
// Official frontend r21702543: MEASURE_CONNECTION_TIMEOUT=15000. Discovery
// supplies an optional override; the engine's startup deadline remains binding.
constexpr int defaultWebsocketConnectionTimeoutMs = 15000;

[[noreturn]] void fail(const QString& phase, const QString& code, const QString& message,
                       bool retryable = false) {
    throw OpError(code, phase, QStringLiteral("yandex: ") + message, retryable);
}

bool loopback(const QString& host) {
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) return true;
    QHostAddress address;
    return address.setAddress(host) && address.isLoopback();
}

QUrl checkedUrl(const QUrl& url, const BackendOptions& options, const QString& phase,
                bool websocket = false) {
    const auto secure = websocket ? QStringLiteral("wss") : QStringLiteral("https");
    const auto local = websocket ? QStringLiteral("ws") : QStringLiteral("http");
    const bool schemeOK = url.scheme() == secure ||
        (options.allowLoopbackHTTP && loopback(url.host()) && url.scheme() == local);
    if (!url.isValid() || !schemeOK || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || !url.fragment().isEmpty()) {
        fail(phase, QStringLiteral("protocol"), QStringLiteral("получен недопустимый URL пробы"));
    }
    return url;
}

QUrl cacheBust(QUrl url) {
    static std::atomic<quint64> counter{0};
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("cb"), QString::number(QDateTime::currentMSecsSinceEpoch(), 36) +
        QStringLiteral("-") + QString::number(counter.fetch_add(1), 36));
    url.setQuery(query);
    return url;
}

QNetworkRequest requestFor(const QUrl& url) {
    QNetworkRequest request(cacheBust(url));
    request.setRawHeader("User-Agent", userAgent);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    return request;
}

void checkStatus(QNetworkReply& reply, const QString& phase) {
    const int status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status == 401 || status == 403)
        fail(phase, QStringLiteral("auth"), QStringLiteral("проба вернула HTTP %1").arg(status), true);
    if (status < 200 || status >= 300) {
        const bool transient = status == 408 || status == 429 || status >= 500;
        fail(phase, transient ? QStringLiteral("unavailable") : QStringLiteral("protocol"),
             QStringLiteral("проба вернула HTTP %1").arg(status), transient);
    }
    const auto encoding = reply.rawHeader("Content-Encoding").trimmed().toLower();
    if (!encoding.isEmpty() && encoding != QByteArrayLiteral("identity"))
        fail(phase, QStringLiteral("protocol"), QStringLiteral("неожиданный Content-Encoding пробы"));
}

void checkNetwork(QNetworkReply& reply, const QString& phase) {
    if (reply.error() == QNetworkReply::NoError) return;
    if (reply.error() == QNetworkReply::TimeoutError)
        fail(phase, QStringLiteral("timeout"), QStringLiteral("истекло время ожидания пробы"), true);
    fail(phase, QStringLiteral("unavailable"), QStringLiteral("сетевой запрос пробы не завершён"), true);
}

std::optional<qint64> contentLength(QNetworkReply& reply, const QString& phase) {
    const auto raw = reply.rawHeader("Content-Length").trimmed();
    if (raw.isEmpty()) return {};
    // Do not accept a sign, fraction, concatenated duplicate values or overflow.
    if (std::any_of(raw.cbegin(), raw.cend(), [](char c) { return c < '0' || c > '9'; }))
        fail(phase, QStringLiteral("protocol"), QStringLiteral("неверный Content-Length пробы"));
    bool valid = false;
    const auto size = raw.toLongLong(&valid);
    if (!valid || size < 0)
        fail(phase, QStringLiteral("protocol"), QStringLiteral("неверный Content-Length пробы"));
    return size;
}

void testTimings(transfer::RunConfig& config, const BackendOptions& options) {
    const bool overrides = options.transferDuration || options.transferWarmup ||
        options.transferReadyGrace || options.transferStartupTimeout;
    if (!overrides) return;
    if (!options.allowLoopbackHTTP || !loopback(options.yandexBase.host()))
        fail(config.phase, QStringLiteral("internal"), QStringLiteral("тестовые интервалы разрешены только для loopback"));
    if (options.transferDuration) config.duration = *options.transferDuration;
    if (options.transferWarmup) config.warmup = *options.transferWarmup;
    if (options.transferReadyGrace) config.readyGrace = *options.transferReadyGrace;
    if (options.transferStartupTimeout) config.startupTimeout = *options.transferStartupTimeout;
}

} // namespace

namespace yandex_transfer {

bool downloadProbe(QNetworkAccessManager& manager, const QUrl& rawUrl,
                   const BackendOptions& options, const transfer::WorkerContext& worker) {
    const QString phase = QStringLiteral("download");
    const auto url = checkedUrl(rawUrl, options, phase);
    if (worker.stopped()) return false;
    auto request = requestFor(url);
    request.setRawHeader("Accept", "application/octet-stream");
    std::unique_ptr<QNetworkReply> reply(manager.get(request));
    reply->setReadBufferSize(chunkSize);
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QElapsedTimer headerClock;
    headerClock.start();
    std::optional<OpError> error;
    bool validated = false;
    bool stopped = false;
    bool headerTimedOut = false;
    qint64 expected = 0;
    qint64 received = 0;
    auto reject = [&](const OpError& e) {
        if (!error) error = e;
        reply->abort();
        loop.quit();
    };
    auto validate = [&] {
        if (validated || error || stopped) return;
        if (!reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()) return;
        try {
            checkStatus(*reply, phase);
            const auto type = reply->rawHeader("Content-Type").split(';').first().trimmed().toLower();
            if (type != QByteArrayLiteral("application/octet-stream"))
                fail(phase, QStringLiteral("protocol"), QStringLiteral("неверный Content-Type пробы скачивания"));
            const auto size = contentLength(*reply, phase);
            if (!size || *size <= 0 || *size > maxDownloadSize)
                fail(phase, QStringLiteral("protocol"), QStringLiteral("неверный размер пробы скачивания"));
            if (url.path().endsWith(QStringLiteral("/probes/50mb")) && *size != nativeDownloadSize)
                fail(phase, QStringLiteral("protocol"), QStringLiteral("неожиданный размер основной пробы скачивания"));
            expected = *size;
            validated = true;
            worker.ready();
        } catch (const OpError& e) { reject(e); }
    };
    auto drain = [&] {
        if (worker.stopped()) {
            stopped = true;
            reply->abort();
            loop.quit();
            return;
        }
        validate();
        if (!validated || error || stopped) return;
        while (reply->bytesAvailable() > 0) {
            if (worker.stopped()) { stopped = true; reply->abort(); loop.quit(); return; }
            const auto chunk = reply->read(chunkSize);
            if (chunk.isEmpty()) break;
            if (chunk.size() > expected - received) {
                reject(OpError(QStringLiteral("protocol"), phase,
                    QStringLiteral("yandex: проба скачивания превысила ожидаемый размер")));
                return;
            }
            received += chunk.size();
            worker.record(chunk.size());
        }
    };
    QObject::connect(reply.get(), &QNetworkReply::metaDataChanged, &loop, validate);
    QObject::connect(reply.get(), &QIODevice::readyRead, &loop, drain);
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (worker.stopped()) {
            stopped = true;
            reply->abort();
            loop.quit();
        } else if (!validated && headerClock.elapsed() >= 10000) {
            headerTimedOut = true;
            reply->abort();
            loop.quit();
        }
    });
    poll.start();
    if (!reply->isFinished()) loop.exec();
    poll.stop();
    if (worker.stopped() || stopped) return false;
    if (error) throw *error;
    if (headerTimedOut)
        fail(phase, QStringLiteral("timeout"), QStringLiteral("истекло время ожидания пробы скачивания"), true);
    drain();
    if (worker.stopped() || stopped) return false;
    if (error) throw *error;
    checkNetwork(*reply, phase);
    if (!validated || received <= 0 || received != expected)
        fail(phase, QStringLiteral("protocol"), QStringLiteral("проба скачивания вернула неполный ответ"));
    return true;
}

void httpUpload(QNetworkAccessManager& manager, const QUrl& rawUrl,
                const BackendOptions& options, const transfer::WorkerContext& worker) {
    const QString phase = QStringLiteral("upload");
    const auto url = checkedUrl(rawUrl, options, phase);
    const QByteArray payload(uploadSize, '\0');
    while (!worker.stopped()) {
        auto request = requestFor(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
        std::unique_ptr<QNetworkReply> reply(manager.post(request, payload));
        reply->setReadBufferSize(chunkSize);
        QEventLoop loop;
        QTimer poll;
        poll.setInterval(10);
        QElapsedTimer clock;
        clock.start();
        std::optional<OpError> error;
        bool validated = false;
        bool stopped = false;
        bool timedOut = false;
        qint64 received = 0;
        std::optional<qint64> expected;
        auto reject = [&](const OpError& e) {
            if (!error) error = e;
            reply->abort();
            loop.quit();
        };
        auto validate = [&] {
            if (validated || error || stopped ||
                !reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).isValid()) return;
            try {
                checkStatus(*reply, phase);
                expected = contentLength(*reply, phase);
                if (expected && *expected > maxUploadResponse)
                    fail(phase, QStringLiteral("protocol"), QStringLiteral("ответ HTTP при отдаче превышает безопасный размер"));
                validated = true;
            } catch (const OpError& e) { reject(e); }
        };
        auto drain = [&] {
            validate();
            if (!validated || error || stopped) return;
            while (reply->bytesAvailable() > 0) {
                const auto chunk = reply->read(std::min(chunkSize, maxUploadResponse + 1 - received));
                if (chunk.isEmpty()) break;
                received += chunk.size();
                if (received > maxUploadResponse) {
                    reject(OpError(QStringLiteral("protocol"), phase,
                        QStringLiteral("yandex: ответ HTTP при отдаче превышает безопасный размер")));
                    return;
                }
            }
        };
        QObject::connect(reply.get(), &QNetworkReply::metaDataChanged, &loop, validate);
        QObject::connect(reply.get(), &QIODevice::readyRead, &loop, drain);
        QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (worker.stopped()) {
                stopped = true;
                reply->abort();
                loop.quit();
            } else if (clock.elapsed() >= 15000) {
                timedOut = true;
                reply->abort();
                loop.quit();
            }
        });
        poll.start();
        if (!reply->isFinished()) loop.exec();
        poll.stop();
        if (worker.stopped() || stopped) return;
        if (error) throw *error;
        if (timedOut)
            fail(phase, QStringLiteral("timeout"), QStringLiteral("истекло время ожидания HTTP при отдаче"), true);
        drain();
        if (error) throw *error;
        checkNetwork(*reply, phase);
        if (!validated || (expected && received != *expected))
            fail(phase, QStringLiteral("protocol"), QStringLiteral("ответ HTTP при отдаче не завершён"));
        // Upload progress / bytesWritten measures queued bytes, never confirmed
        // throughput. The complete successful HTTP response confirms this block.
        worker.ready();
        worker.record(payload.size());
    }
}

void websocketUpload(const YandexUploadProbe& probe, const BackendOptions& options,
                     std::chrono::milliseconds sessionDuration,
                     const transfer::WorkerContext& worker) {
    const QString phase = QStringLiteral("upload");
    auto url = checkedUrl(probe.websocketUrl, options, phase, true);
    if (probe.websocketConnectionTimeoutMs < 0 || probe.websocketConnectionTimeoutMs > 30000)
        fail(phase, QStringLiteral("protocol"), QStringLiteral("неверный таймаут подключения WebSocket"));
    if (worker.stopped()) return;
    QUrlQuery query(url);
    query.removeAllQueryItems(QStringLiteral("type"));
    query.removeAllQueryItems(QStringLiteral("duration"));
    query.addQueryItem(QStringLiteral("type"), QStringLiteral("upload"));
    query.addQueryItem(QStringLiteral("duration"), QString::number(std::max<qint64>(1,
        (sessionDuration.count() + 999) / 1000)));
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", userAgent);
    request.setRawHeader("Origin", options.yandexBase.toString(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment).toUtf8());
    QWebSocket socket;
    socket.setMaxAllowedIncomingMessageSize(maxAckSize);
    socket.setMaxAllowedIncomingFrameSize(maxAckSize);
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QTimer writer;
    writer.setInterval(1);
    QElapsedTimer connectClock;
    connectClock.start();
    std::optional<OpError> error;
    bool connected = false;
    bool stopped = false;
    qint64 sent = 0;
    qint64 acknowledged = 0;
    const QByteArray payload(chunkSize, '\0');
    auto reject = [&](const QString& code, const QString& message, bool retryable = false) {
        if (!error) error.emplace(code, phase, QStringLiteral("yandex: ") + message, retryable);
        writer.stop();
        socket.abort();
        loop.quit();
    };
    auto protocolClose = [&] {
        const auto code = socket.closeCode();
        return code == QWebSocketProtocol::CloseCodeTooMuchData ||
            code == QWebSocketProtocol::CloseCodeProtocolError ||
            code == QWebSocketProtocol::CloseCodeWrongDatatype ||
            code == QWebSocketProtocol::CloseCodeDatatypeNotSupported;
    };
    auto send = [&] {
        if (error || stopped || !connected || socket.state() != QAbstractSocket::ConnectedState) return;
        // As in the official frontend, bound the transport queue to 1 MiB.
        // Each timer turn fills at most that queue and lets ACKs/cancellation run.
        const auto count = (maxBufferedBytes - socket.bytesToWrite()) / chunkSize;
        for (qint64 i = 0; i < count && !worker.stopped(); ++i) {
            const auto bytes = socket.sendBinaryMessage(payload);
            if (bytes != payload.size() || sent > std::numeric_limits<qint64>::max() - bytes) {
                const bool protocol = protocolClose();
                reject(protocol ? QStringLiteral("protocol") : QStringLiteral("unavailable"),
                       protocol ? QStringLiteral("неверное подтверждение WebSocket") :
                           QStringLiteral("не удалось отправить блок WebSocket"), !protocol);
                return;
            }
            sent += bytes;
        }
    };
    QObject::connect(&socket, &QWebSocket::connected, &loop, [&] {
        connected = true;
        writer.start();
        send();
    });
    QObject::connect(&writer, &QTimer::timeout, &loop, send);
    QObject::connect(&socket, &QWebSocket::textMessageReceived, &loop, [&](const QString& message) {
        if (error || stopped || worker.stopped()) return;
        const auto bytes = message.toUtf8();
        QJsonParseError parseError;
        const auto doc = QJsonDocument::fromJson(bytes, &parseError);
        if (bytes.size() > maxAckSize || parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            reject(QStringLiteral("protocol"), QStringLiteral("неверное подтверждение WebSocket"));
            return;
        }
        const auto ack = doc.object();
        const auto value = ack.value(QStringLiteral("b"));
        const auto amount = value.toDouble(-1);
        const auto confirmed = value.toInteger(-1);
        const auto available = sent - acknowledged;
        if (ack.value(QStringLiteral("k")).toString() != QStringLiteral("u") || !value.isDouble() ||
            !std::isfinite(amount) || amount < 0 || std::floor(amount) != amount ||
            confirmed < 0 || confirmed > available) {
            reject(QStringLiteral("protocol"), QStringLiteral("неверное подтверждение WebSocket"));
            return;
        }
        if (confirmed == 0) return;
        acknowledged += confirmed;
        worker.ready();
        worker.record(confirmed);
    });
    QObject::connect(&socket, &QWebSocket::binaryMessageReceived, &loop, [&](const QByteArray&) {
        reject(QStringLiteral("protocol"), QStringLiteral("неожиданный тип подтверждения WebSocket"));
    });
    QObject::connect(&socket, &QWebSocket::aboutToClose, &loop, [&] {
        if (!stopped && !error && !worker.stopped() && protocolClose()) {
            // Qt rejects oversized/invalid frames before messageReceived. Freeze
            // the typed close reason before a queued writer sees a closed socket.
            error.emplace(QStringLiteral("protocol"), phase, QStringLiteral("yandex: неверное подтверждение WebSocket"));
            writer.stop();
            loop.quit();
        }
    });
    QObject::connect(&socket, &QWebSocket::disconnected, &loop, [&] {
        if (!stopped && !error && !worker.stopped()) {
            const bool protocol = protocolClose();
            error.emplace(protocol ? QStringLiteral("protocol") : QStringLiteral("unavailable"), phase,
                protocol ? QStringLiteral("yandex: неверное подтверждение WebSocket") :
                    QStringLiteral("yandex: соединение WebSocket закрыто раньше завершения отдачи"), !protocol);
        }
        loop.quit();
    });
    QObject::connect(&socket, &QWebSocket::errorOccurred, &loop, [&](QAbstractSocket::SocketError code) {
        if (stopped || worker.stopped() || error) return;
        const bool protocol = protocolClose();
        reject(protocol ? QStringLiteral("protocol") :
                   code == QAbstractSocket::SocketTimeoutError ? QStringLiteral("timeout") : QStringLiteral("unavailable"),
               protocol ? QStringLiteral("неверное подтверждение WebSocket") : QStringLiteral("ошибка соединения WebSocket"), !protocol);
    });
    const int connectTimeout = probe.websocketConnectionTimeoutMs > 0 ?
        probe.websocketConnectionTimeoutMs : defaultWebsocketConnectionTimeoutMs;
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (worker.stopped()) {
            stopped = true;
            writer.stop();
            socket.abort();
            loop.quit();
        } else if (!connected && connectClock.elapsed() >= connectTimeout) {
            reject(QStringLiteral("timeout"), QStringLiteral("истекло время подключения WebSocket"), true);
        }
    });
    poll.start();
    socket.open(request);
    if (!error && !worker.stopped()) loop.exec();
    writer.stop();
    poll.stop();
    socket.abort();
    if (worker.stopped() || stopped) return;
    if (error) throw *error;
    fail(phase, QStringLiteral("unavailable"), QStringLiteral("соединение WebSocket завершено раньше отдачи"), true);
}

} // namespace yandex_transfer

QJsonObject yandexDownload(const Config& config, const Cancellation& cancellation,
                          const BackendOptions& options, const QList<QUrl>& urls,
                          transfer::Progress progress) {
    if (urls.isEmpty())
        fail(QStringLiteral("download"), QStringLiteral("internal"), QStringLiteral("сначала необходимо выбрать сервер"));
    auto runConfig = transfer::configure(config, QStringLiteral("download"), static_cast<int>(urls.size()),
        transfer::Milliseconds{750}, transfer::Milliseconds{8000}, transfer::Milliseconds{400});
    testTimings(runConfig, options);
    for (const auto& url : urls) checkedUrl(url, options, runConfig.phase);
    const auto result = transfer::run(runConfig, cancellation,
        [&](int index, int, const transfer::WorkerContext& worker) {
            QNetworkAccessManager manager;
            const auto& url = urls[index % urls.size()];
            while (!worker.stopped() && yandex_transfer::downloadProbe(manager, url, options, worker)) {}
        }, std::move(progress));
    return transfer::phaseResult(result, QStringLiteral("http"));
}

QJsonObject yandexUpload(const Config& config, const Cancellation& cancellation,
                        const BackendOptions& options, const QList<YandexUploadProbe>& probes,
                        transfer::Progress progress) {
    if (probes.isEmpty())
        fail(QStringLiteral("upload"), QStringLiteral("internal"), QStringLiteral("сначала необходимо выбрать сервер"));
    auto runConfig = transfer::configure(config, QStringLiteral("upload"), static_cast<int>(probes.size()),
        transfer::Milliseconds{750}, transfer::Milliseconds{10000}, transfer::Milliseconds{400});
    testTimings(runConfig, options);
    for (const auto& probe : probes) {
        checkedUrl(probe.postUrl, options, runConfig.phase);
        if (!probe.websocketUrl.isEmpty()) checkedUrl(probe.websocketUrl, options, runConfig.phase, true);
    }
    // Server duration starts on connection, before readiness, warm-up or any
    // additional worker grace. Cover all of those intervals, not just measurement.
    const auto sessionDuration = runConfig.startupTimeout + runConfig.warmup +
        runConfig.duration + 2 * runConfig.readyGrace + transfer::Milliseconds{2000};
    const auto result = transfer::run(runConfig, cancellation,
        [&](int index, int, const transfer::WorkerContext& worker) {
            const auto& probe = probes[index % probes.size()];
            if (!probe.websocketUrl.isEmpty()) {
                try {
                    yandex_transfer::websocketUpload(probe, options, sessionDuration, worker);
                    return;
                } catch (const OpError&) {
                    if (worker.stopped()) return;
                    // HTTP is the first-party fallback advertised by this probe.
                    // It confirms whole blocks only after successful responses.
                }
            }
            QNetworkAccessManager manager;
            yandex_transfer::httpUpload(manager, probe.postUrl, options, worker);
        }, std::move(progress));
    return transfer::phaseResult(result, QStringLiteral("websocket_or_http"));
}

} // namespace puls
