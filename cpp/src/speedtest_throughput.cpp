#include "puls/transfer_network.hpp"

#include <QDateTime>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QHostAddress>
#include <QRegularExpression>
#include <QTimer>
#include <QThread>
#include <QUrlQuery>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <mutex>

namespace puls {
namespace {
constexpr qsizetype maxPageBytes = 8 * 1024 * 1024;
constexpr qsizetype maxJWTBytes = 64 * 1024;
constexpr qsizetype maxUploadReplyBytes = 1 * 1024 * 1024;
constexpr qint64 uploadBlockBytes = 2 * 1024 * 1024;
constexpr int requestTimeoutMs = 30000;
constexpr auto userAgent = "Mozilla/5.0 (compatible; Puls; +https://github.com/Cheviiot/Puls)";

[[noreturn]] void fail(const QString& phase, const QString& code, const QString& message,
                       bool retryable = false) {
    throw OpError(code, phase, QStringLiteral("speedtest.ru: ") + message, retryable);
}

QString mimeType(const QByteArray& header) {
    return header.split(';').value(0).trimmed().toLower();
}

bool sameOrigin(const QUrl& a, const QUrl& b) {
    const auto port = [](const QUrl& u) { return u.port(u.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443 : 80); };
    return a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0 &&
           a.host().compare(b.host(), Qt::CaseInsensitive) == 0 && port(a) == port(b);
}

bool loopbackHost(const QString& host) {
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) return true;
    QHostAddress address;
    return address.setAddress(host) && address.isLoopback();
}

void validateUrl(const QUrl& url, const BackendOptions& options, const QString& phase) {
    const bool https = url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0;
    const bool testHttp = options.allowLoopbackHTTP && url.scheme() == QStringLiteral("http") && loopbackHost(url.host());
    if (!url.isValid() || (!https && !testHttp) || url.host().isEmpty() || !url.userInfo().isEmpty() || !url.fragment().isEmpty())
        fail(phase, QStringLiteral("protocol"), QStringLiteral("получен недопустимый URL сервера измерения"), true);
}

QNetworkRequest requestFor(const QUrl& url, const QByteArray& accept = "application/json") {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", userAgent);
    request.setRawHeader("Accept", accept);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    return request;
}

struct Response {
    int status = 0;
    QByteArray type;
    QByteArray encoding;
    QByteArray contentLength;
    QByteArray body;
    qint64 bytesRead = 0;
    QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
    bool canceled = false;
    bool timedOut = false;
    bool oversized = false;
    bool headersRejected = false;
};

Response perform(QNetworkAccessManager& manager, QNetworkRequest request, QByteArray upload,
                 qsizetype maxBody, int timeoutMs, const std::function<bool()>& stopped,
                 const QString& phase, const std::function<bool(const Response&)>& headers = {},
                 const std::function<void(qint64)>& onBytes = {}, bool countOnly = false) {
    if (stopped && stopped()) fail(phase, QStringLiteral("canceled"), QStringLiteral("операция отменена"));
    QNetworkReply* reply = upload.isNull() ? manager.get(request) : manager.post(request, upload);
    reply->setReadBufferSize(qMax<qint64>(65536, qMin<qint64>(maxBody + 1, 65536)));
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QElapsedTimer elapsed;
    elapsed.start();
    Response response;
    bool headersSent = false;
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&] {
        if (headersSent) return;
        headersSent = true;
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.type = reply->rawHeader("Content-Type");
        response.encoding = reply->rawHeader("Content-Encoding");
        response.contentLength = reply->rawHeader("Content-Length");
        if (headers && !headers(response)) {
            response.headersRejected = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
        if (maxBody < 0) {
            const auto chunk = reply->read(65536);
            response.bytesRead += chunk.size();
            if (onBytes && !chunk.isEmpty()) onBytes(chunk.size());
            return;
        }
        const qint64 limit = response.status < 200 || response.status >= 300
            ? qMin<qint64>(maxBody, maxUploadReplyBytes) : maxBody;
        const qint64 consumed = countOnly ? response.bytesRead : response.body.size();
        const QByteArray chunk = reply->read(qMin<qint64>(65536, limit + 1 - consumed));
        if (!countOnly) response.body.append(chunk);
        response.bytesRead += chunk.size();
        if ((countOnly && response.bytesRead > limit) || (!countOnly && response.body.size() > limit)) {
            response.oversized = true;
            reply->abort();
            return;
        }
        if (onBytes && !chunk.isEmpty()) onBytes(chunk.size());
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (stopped && stopped()) {
            response.canceled = true;
            reply->abort();
            loop.quit();
        } else if (elapsed.elapsed() >= timeoutMs) {
            response.timedOut = true;
            reply->abort();
            loop.quit();
        }
    });
    poll.start();
    loop.exec();
    poll.stop();
    if (!headersSent) {
        response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.type = reply->rawHeader("Content-Type");
        response.encoding = reply->rawHeader("Content-Encoding");
        response.contentLength = reply->rawHeader("Content-Length");
        if (headers && response.status != 0 && !headers(response)) response.headersRejected = true;
    }
    if (maxBody >= 0 && !response.oversized) {
        const qint64 limit = response.status < 200 || response.status >= 300
            ? qMin<qint64>(maxBody, maxUploadReplyBytes) : maxBody;
        const QByteArray rest = reply->read(limit + 1 - (countOnly ? response.bytesRead : response.body.size()));
        if (!countOnly) response.body.append(rest);
        response.bytesRead += rest.size();
        response.oversized = countOnly ? response.bytesRead > limit : response.body.size() > limit;
        if (!response.oversized && onBytes && !rest.isEmpty()) onBytes(rest.size());
    }
    response.networkError = reply->error();
    reply->deleteLater();
    return response;
}

void validateTransport(const Response& r, const QString& phase, const QString& operation) {
    if (r.canceled) fail(phase, QStringLiteral("canceled"), operation + QStringLiteral(": операция отменена"));
    if (r.timedOut || r.networkError == QNetworkReply::TimeoutError)
        fail(phase, QStringLiteral("timeout"), operation + QStringLiteral(": истекло время ожидания"), true);
    if (r.status == 401 || r.status == 403) fail(phase, QStringLiteral("auth"), operation + QStringLiteral(": сервер отклонил авторизацию"), true);
    if (r.status >= 200 && r.status < 300 && r.oversized)
        fail(phase, QStringLiteral("protocol"), operation + QStringLiteral(": превышен безопасный размер ответа"));
    if (r.headersRejected) fail(phase, QStringLiteral("protocol"), operation + QStringLiteral(": заголовки ответа не прошли проверку"), true);
    if (r.status == 0 && r.networkError != QNetworkReply::NoError)
        fail(phase, QStringLiteral("unavailable"), operation + QStringLiteral(": сетевой запрос не завершён"), true);
}

void validateStatus(const Response& r, const QString& phase, const QString& operation) {
    validateTransport(r, phase, operation);
    if (r.status < 200 || r.status >= 300) {
        const bool transient = r.status == 408 || r.status == 429 || r.status >= 500;
        fail(phase, transient ? QStringLiteral("unavailable") : QStringLiteral("protocol"),
             operation + QStringLiteral(": HTTP статус %1").arg(r.status), transient);
    }
    if (r.networkError != QNetworkReply::NoError)
        fail(phase, QStringLiteral("unavailable"), operation + QStringLiteral(": сетевой запрос не завершён"), true);
}

QUrl apiUrl(const BackendOptions& options, const QString& path) {
    QUrl url = options.speedtestBase;
    url.setPath(path);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("t"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    url.setQuery(query);
    return url;
}

QString fetchToken(QNetworkAccessManager& manager, const BackendOptions& options,
                   const QString& key, const std::function<bool()>& stopped,
                   const QString& phase, int* statusOut = nullptr) {
    auto request = requestFor(apiUrl(options, QStringLiteral("/api/server/gentoken")));
    request.setRawHeader("x-api-key", key.toUtf8());
    const auto response = perform(manager, request, QByteArrayLiteral(""), maxJWTBytes, requestTimeoutMs, stopped, phase);
    if (statusOut) *statusOut = response.status;
    validateStatus(response, phase, QStringLiteral("gentoken"));
    if (mimeType(response.type) != QByteArrayLiteral("application/json"))
        fail(phase, QStringLiteral("protocol"), QStringLiteral("gentoken: неверный Content-Type"), true);
    if (!response.encoding.trimmed().isEmpty() && response.encoding.trimmed().toLower() != QByteArrayLiteral("identity"))
        fail(phase, QStringLiteral("protocol"), QStringLiteral("gentoken: неожиданный Content-Encoding"), true);
    QJsonParseError jsonError;
    const auto doc = QJsonDocument::fromJson(response.body, &jsonError);
    const auto token = doc.isObject() ? doc.object().value(QStringLiteral("token")).toString() : QString();
    const auto parts = token.split('.');
    static const QRegularExpression tokenPattern(QStringLiteral("^[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]+\\.[A-Za-z0-9_-]+$"));
    if (jsonError.error != QJsonParseError::NoError || !doc.isObject() || token.size() < 16 || parts.size() != 3 ||
        !tokenPattern.match(token).hasMatch())
        fail(phase, QStringLiteral("auth"), QStringLiteral("gentoken вернул неверный токен"), true);
    return token;
}

QString fetchBrowserKey(QNetworkAccessManager& manager, const BackendOptions& options,
                        const std::function<bool()>& stopped, const QString& phase) {
    QElapsedTimer budget;
    budget.start();
    const auto page = perform(manager, requestFor(options.speedtestBase, "text/html"), QByteArray(), maxPageBytes,
                              8000, stopped, phase);
    validateStatus(page, phase, QStringLiteral("страница speedtest.ru"));
    if (mimeType(page.type) != QByteArrayLiteral("text/html") && mimeType(page.type) != QByteArrayLiteral("application/xhtml+xml"))
        fail(phase, QStringLiteral("protocol"), QStringLiteral("страница speedtest.ru: неверный Content-Type"), true);
    if (!page.encoding.trimmed().isEmpty() && page.encoding.trimmed().toLower() != QByteArrayLiteral("identity"))
        fail(phase, QStringLiteral("protocol"), QStringLiteral("страница speedtest.ru: неожиданный Content-Encoding"), true);
    const QRegularExpression assets(QStringLiteral("src=[\\\"']([^\\\"']+\\.js[^\\\"']*)[\\\"']"));
    const auto matches = assets.globalMatch(QString::fromUtf8(page.body));
    const QRegularExpression keyPattern(QStringLiteral("QMSLibrary\\(\\{\\s*id\\s*:\\s*[\\\"']([A-Za-z0-9_-]{16,128})[\\\"']"));
    QUrl origin = options.speedtestBase;
    int count = 0;
    auto it = matches;
    while (it.hasNext() && count++ < 32) {
        const int remainingMs = 15000 - static_cast<int>(budget.elapsed());
        if (remainingMs <= 0) break;
        const auto match = it.next();
        const QUrl assetUrl = origin.resolved(QUrl(match.captured(1), QUrl::StrictMode));
        if (!assetUrl.isValid() || !assetUrl.userInfo().isEmpty() || !assetUrl.fragment().isEmpty() ||
            !sameOrigin(origin, assetUrl)) continue;
        const auto asset = perform(manager, requestFor(assetUrl, "*/*"), QByteArray(), maxPageBytes,
                                   qMin(remainingMs, 5000), stopped, phase);
        if (asset.status < 200 || asset.status >= 300 || asset.canceled || asset.timedOut || asset.oversized ||
            asset.networkError != QNetworkReply::NoError ||
            (!asset.encoding.trimmed().isEmpty() && asset.encoding.trimmed().toLower() != QByteArrayLiteral("identity"))) continue;
        const auto media = mimeType(asset.type);
        if (media != QByteArrayLiteral("application/javascript") && media != QByteArrayLiteral("text/javascript") &&
            media != QByteArrayLiteral("text/plain")) continue;
        const auto found = keyPattern.match(QString::fromUtf8(asset.body));
        if (found.hasMatch()) return found.captured(1);
    }
    fail(phase, QStringLiteral("auth"), QStringLiteral("не удалось обновить browser key"), true);
}

QString ensureJwt(SpeedtestSession& session, const BackendOptions& options, const Cancellation& cancellation,
                  const QString& phase, const std::function<bool()>& stopped,
                  bool refresh = false, const QString& previous = QString()) {
    std::unique_lock<std::mutex> lock(session.mutex, std::defer_lock);
    while (!lock.try_lock()) {
        if (stopped && stopped()) fail(phase, QStringLiteral("canceled"), QStringLiteral("операция отменена"));
        QThread::msleep(1);
    }
    if (cancellation.canceled() || (stopped && stopped()))
        fail(phase, QStringLiteral("canceled"), QStringLiteral("операция отменена"));
    if (!session.jwt.isEmpty() && (!refresh || previous.isEmpty() || session.jwt != previous)) return session.jwt;
    QNetworkAccessManager manager;
    int status = 0;
    QString token;
    try {
        token = fetchToken(manager, options, session.browserKey, stopped, phase, &status);
    } catch (const OpError&) {
        if (status != 401 && status != 403) throw;
        if (session.keyRefreshAttempted)
            fail(phase, QStringLiteral("auth"), QStringLiteral("gentoken отклонил browser key после единственного обновления"), true);
        session.keyRefreshAttempted = true;
        session.browserKey = fetchBrowserKey(manager, options, stopped, phase);
        token = fetchToken(manager, options, session.browserKey, stopped, phase, &status);
    }
    session.jwt = token;
    return token;
}

QUrl transferUrl(const QUrl& base, const QString& path, const QString& size = QString()) {
    static std::atomic<quint64> nonce{0};
    QUrl url = base;
    QString basePath = url.path();
    if (!basePath.endsWith('/')) basePath += '/';
    url.setPath(basePath + path);
    QUrlQuery query;
    if (!size.isEmpty()) query.addQueryItem(QStringLiteral("ckSize"), size);
    query.addQueryItem(QStringLiteral("r"), QStringLiteral("%1-%2")
        .arg(QDateTime::currentMSecsSinceEpoch()).arg(nonce.fetch_add(1, std::memory_order_relaxed)));
    url.setQuery(query);
    return url;
}

QUrl serverAt(const QUrl& raw, const BackendOptions& options, const QString& phase) {
    QUrl url = raw;
    validateUrl(url, options, phase);
    return url;
}

QList<QUrl> checkedServers(const QList<QUrl>& servers, const BackendOptions& options, const QString& phase) {
    QList<QUrl> result;
    for (const auto& url : servers) result.append(serverAt(url, options, phase));
    if (result.isEmpty()) fail(phase, QStringLiteral("internal"), QStringLiteral("список серверов измерения пуст"));
    return result;
}

int nextChunk(qint64 bytes, qint64 elapsedMs) {
    if (bytes <= 0 || elapsedMs <= 0) return 25;
    const double target = static_cast<double>(bytes) * 6000.0 / static_cast<double>(elapsedMs) / 1000000.0;
    if (!std::isfinite(target) || target >= 250) return 250;
    if (target <= 25) return 25;
    return qBound(25, qRound(target), 250);
}

void recordDownload(QNetworkAccessManager& manager, const QUrl& server, int chunkMB, const QString& token, const BackendOptions& options,
                    const transfer::WorkerContext& worker, const QString& phase, qint64& received) {
    if (chunkMB < 1 || chunkMB > 250) fail(phase, QStringLiteral("protocol"), QStringLiteral("неверный размер блока скачивания"));
    const qint64 expected = static_cast<qint64>(chunkMB) * 1000000;
    auto request = requestFor(transferUrl(server, QStringLiteral("download.php"), QString::number(chunkMB)), "application/octet-stream");
    request.setRawHeader("jwt", token.toUtf8());
    request.setRawHeader("Accept-Encoding", "identity");
    bool headersReady = false;
    const auto response = perform(manager, request, QByteArray(), expected, requestTimeoutMs,
                                  worker.stopped, phase, [&](const Response& headers) {
        if (headers.status < 200 || headers.status >= 300) return true;
        if (mimeType(headers.type) != QByteArrayLiteral("application/octet-stream"))
            return false;
        else if (!headers.encoding.trimmed().isEmpty() && headers.encoding.trimmed().toLower() != QByteArrayLiteral("identity"))
            return false;
        else if (!headers.contentLength.isEmpty()) {
            static const QRegularExpression decimalLength(QStringLiteral("^[0-9]{1,12}$"));
            if (!decimalLength.match(QString::fromLatin1(headers.contentLength)).hasMatch()) return false;
            bool ok = false;
            const qint64 length = headers.contentLength.toLongLong(&ok);
            if (!ok || length != expected) return false;
        }
        if (!worker.stopped()) {
            headersReady = true;
            worker.ready();
        }
        return true;
    }, [&](qint64 count) {
        if (headersReady && received + count <= expected) {
            received += count;
            worker.record(count);
        }
    }, true);
    Q_UNUSED(options);
    validateStatus(response, phase, QStringLiteral("download"));
    if (response.headersRejected) fail(phase, QStringLiteral("protocol"), QStringLiteral("download: заголовки ответа не прошли проверку"), true);
    if (!headersReady) fail(phase, QStringLiteral("protocol"), QStringLiteral("download: сервер не подтвердил заголовки"), true);
    if (received != expected)
        fail(phase, QStringLiteral("protocol"), QStringLiteral("download: получено %1 байт, ожидалось %2").arg(received).arg(expected), true);
}

} // namespace

QString refreshSpeedtestBrowserKey(SpeedtestSession& session, const BackendOptions& options,
                                   const Cancellation& cancellation, const QString& phase) {
    std::unique_lock<std::mutex> lock(session.mutex);
    if (session.keyRefreshAttempted)
        fail(phase, QStringLiteral("auth"), QStringLiteral("browser key уже обновлялся"), true);
    session.keyRefreshAttempted = true;
    QNetworkAccessManager manager;
    const auto key = fetchBrowserKey(manager, options, [&] { return cancellation.canceled(); }, phase);
    session.browserKey = key;
    return key;
}

QJsonObject speedtestDownload(const Config& config, const Cancellation& cancellation,
                              const BackendOptions& options, const QList<QUrl>& inputServers,
                              SpeedtestSession& session, transfer::Progress progress) {
    const auto servers = checkedServers(inputServers, options, QStringLiteral("download"));
    (void)ensureJwt(session, options, cancellation, QStringLiteral("download"), [&] { return cancellation.canceled(); });
    const int nativeWorkers = static_cast<int>(qMin<qsizetype>(16, servers.size() * 2));
    auto runConfig = transfer::configure(config, QStringLiteral("download"), nativeWorkers,
        std::chrono::milliseconds(2000), std::chrono::milliseconds(10000), std::chrono::milliseconds(500));
    if (options.transferWarmup) runConfig.warmup = *options.transferWarmup;
    if (options.transferStartupTimeout) runConfig.startupTimeout = *options.transferStartupTimeout;
    if (options.transferReadyGrace) runConfig.readyGrace = *options.transferReadyGrace;
    if (options.transferDuration) runConfig.duration = *options.transferDuration;
    auto result = transfer::run(runConfig, cancellation, [&](int index, int attempt, const transfer::WorkerContext& worker) {
        QNetworkAccessManager manager;
        const QUrl server = servers.at((index + attempt) % servers.size());
        int chunkMB = 25;
        while (!worker.stopped()) {
            const auto token = ensureJwt(session, options, cancellation, QStringLiteral("download"), worker.stopped);
            qint64 received = 0;
            QElapsedTimer timer;
            timer.start();
            try {
                recordDownload(manager, server, chunkMB, token, options, worker, QStringLiteral("download"), received);
            } catch (const OpError& error) {
                if (error.code == QStringLiteral("auth")) {
                    const auto fresh = ensureJwt(session, options, cancellation, QStringLiteral("download"), worker.stopped, true, token);
                    recordDownload(manager, server, chunkMB, fresh, options, worker, QStringLiteral("download"), received);
                } else throw;
            }
            chunkMB = nextChunk(received, timer.elapsed());
        }
    }, progress);
    return transfer::phaseResult(result, QStringLiteral("http"));
}

QJsonObject speedtestUpload(const Config& config, const Cancellation& cancellation,
                            const BackendOptions& options, const QList<QUrl>& inputServers,
                            SpeedtestSession& session, transfer::Progress progress) {
    const auto servers = checkedServers(inputServers, options, QStringLiteral("upload"));
    (void)ensureJwt(session, options, cancellation, QStringLiteral("upload"), [&] { return cancellation.canceled(); });
    const QByteArray payload(static_cast<int>(uploadBlockBytes), '\0');
    const int nativeWorkers = static_cast<int>(qMin<qsizetype>(16, servers.size() * 2));
    auto runConfig = transfer::configure(config, QStringLiteral("upload"), nativeWorkers,
        std::chrono::milliseconds(4000), std::chrono::milliseconds(12000), std::chrono::milliseconds(500));
    if (options.transferWarmup) runConfig.warmup = *options.transferWarmup;
    if (options.transferStartupTimeout) runConfig.startupTimeout = *options.transferStartupTimeout;
    if (options.transferReadyGrace) runConfig.readyGrace = *options.transferReadyGrace;
    if (options.transferDuration) runConfig.duration = *options.transferDuration;
    auto result = transfer::run(runConfig, cancellation, [&](int index, int attempt, const transfer::WorkerContext& worker) {
        QNetworkAccessManager manager;
        const QUrl server = servers.at((index + attempt) % servers.size());
        while (!worker.stopped()) {
            const auto token = ensureJwt(session, options, cancellation, QStringLiteral("upload"), worker.stopped);
            auto request = requestFor(transferUrl(server, QStringLiteral("upload.php")), "*/*");
            request.setRawHeader("jwt", token.toUtf8());
            request.setRawHeader("Content-Type", "application/octet-stream");
            Response response;
            try {
                response = perform(manager, request, payload, maxUploadReplyBytes, requestTimeoutMs, worker.stopped, QStringLiteral("upload"));
                validateStatus(response, QStringLiteral("upload"), QStringLiteral("upload"));
                if (!response.encoding.trimmed().isEmpty() && response.encoding.trimmed().toLower() != QByteArrayLiteral("identity"))
                    fail(QStringLiteral("upload"), QStringLiteral("protocol"), QStringLiteral("upload: неожиданный Content-Encoding"), true);
            } catch (const OpError& error) {
                if (error.code != QStringLiteral("auth")) throw;
                const auto fresh = ensureJwt(session, options, cancellation, QStringLiteral("upload"), worker.stopped, true, token);
                request.setRawHeader("jwt", fresh.toUtf8());
                response = perform(manager, request, payload, maxUploadReplyBytes, requestTimeoutMs, worker.stopped, QStringLiteral("upload"));
                validateStatus(response, QStringLiteral("upload"), QStringLiteral("upload"));
                if (!response.encoding.trimmed().isEmpty() && response.encoding.trimmed().toLower() != QByteArrayLiteral("identity"))
                    fail(QStringLiteral("upload"), QStringLiteral("protocol"), QStringLiteral("upload: неожиданный Content-Encoding"), true);
            }
            if (!worker.stopped()) {
                worker.ready();
                worker.record(uploadBlockBytes);
            }
        }
    }, progress);
    return transfer::phaseResult(result, QStringLiteral("http"));
}

} // namespace puls
