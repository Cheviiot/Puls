#include "puls/core.hpp"
#include "puls/network.hpp"

#include <QElapsedTimer>
#include <QDateTime>
#include <QEventLoop>
#include <QAbstractSocket>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QThread>
#include <QUrlQuery>
#include <QWebSocket>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <vector>

namespace puls {
namespace {
constexpr auto userAgent = "Mozilla/5.0 (compatible; Puls; +https://github.com/Cheviiot/Puls)";
constexpr auto browserKey = "5f3287b55fbcd8076919114885f8f3f7";
constexpr qsizetype maxYandexDiscovery = 2 * 1024 * 1024;
constexpr qsizetype maxYandexPage = 8 * 1024 * 1024;
constexpr qsizetype maxSpeedDiscovery = 4 * 1024 * 1024;
constexpr qsizetype maxJSONConnection = 64 * 1024;
constexpr qsizetype maxPingBody = 64 * 1024;
constexpr qsizetype maxWebSocketMessage = 64 * 1024;

struct HttpResponse {
    int status = 0;
    QByteArray contentType;
    QByteArray contentEncoding;
    QByteArray body;
    qint64 elapsedMs = 0;
    QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
    bool canceled = false;
    bool timedOut = false;
    bool oversized = false;
};

[[noreturn]] void fail(const QString& service, const QString& phase, const QString& code,
                       const QString& message, bool retryable = false) {
    throw OpError(code, phase, service + QStringLiteral(": ") + message, retryable);
}

void requireActive(const Cancellation& cancellation, const QString& phase) {
    if (cancellation.canceled()) fail(QStringLiteral("network"), phase, QStringLiteral("canceled"), QStringLiteral("операция отменена"));
}

bool isLoopback(const QString& host) {
    if (host.compare(QStringLiteral("localhost"), Qt::CaseInsensitive) == 0) return true;
    QHostAddress address;
    return address.setAddress(host) && address.isLoopback();
}

QUrl checkedUrl(const QString& raw, const QString& scheme, const QString& phase,
                bool allowLoopbackHTTP = false) {
    const QUrl url(raw, QUrl::StrictMode);
    const bool exactScheme = url.scheme().compare(scheme, Qt::CaseInsensitive) == 0;
    const bool localHttp = allowLoopbackHTTP &&
        ((scheme == QStringLiteral("https") && url.scheme() == QStringLiteral("http")) ||
         (scheme == QStringLiteral("wss") && url.scheme() == QStringLiteral("ws"))) && isLoopback(url.host());
    if (!url.isValid() || (!exactScheme && !localHttp) ||
        url.host().isEmpty() || !url.userInfo().isEmpty() || !url.fragment().isEmpty()) {
        fail(QStringLiteral("network"), phase, QStringLiteral("protocol"), QStringLiteral("получен недопустимый URL пробы"), true);
    }
    return url;
}

QNetworkRequest requestFor(const QUrl& url, const QByteArray& accept = "application/json") {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", userAgent);
    request.setRawHeader("Accept", accept);
    request.setRawHeader("Accept-Encoding", "identity");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    return request;
}

HttpResponse perform(QNetworkAccessManager& manager, QNetworkRequest request, qsizetype maxBytes,
                     int timeoutMs, const Cancellation& cancellation, const QString& phase) {
    requireActive(cancellation, phase);
    QNetworkReply* reply = manager.get(request);
    reply->setReadBufferSize(maxBytes + 1);
    QEventLoop loop;
    QTimer timer;
    timer.setInterval(10);
    QElapsedTimer elapsed;
    elapsed.start();
    HttpResponse response;
    bool canceled = false;
    bool timedOut = false;
    bool oversized = false;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QIODevice::readyRead, &loop, [&] {
        const auto chunk = reply->read(maxBytes + 1 - response.body.size());
        response.body.append(chunk);
        if (response.body.size() > maxBytes) {
            oversized = true;
            reply->abort();
        }
    });
    QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
        if (cancellation.canceled()) {
            canceled = true;
            reply->abort();
            loop.quit();
        } else if (elapsed.elapsed() >= timeoutMs) {
            timedOut = true;
            reply->abort();
            loop.quit();
        }
    });
    timer.start();
    loop.exec();
    timer.stop();
    // The body is drained incrementally above to keep the reply buffer bounded.
    response.elapsedMs = elapsed.elapsed();
    response.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    response.contentType = reply->rawHeader("Content-Type");
    response.contentEncoding = reply->rawHeader("Content-Encoding");
    response.body.append(reply->read(maxBytes + 1 - response.body.size()));
    response.canceled = canceled;
    response.timedOut = timedOut;
    response.oversized = oversized || response.body.size() > maxBytes;
    response.networkError = reply->error();
    reply->deleteLater();
    return response;
}

QJsonDocument parseJson(const QByteArray& data, const QString& service, const QString& phase,
                        const QString& operation, qsizetype limit, bool retryable = true) {
    if (data.size() > limit) fail(service, phase, QStringLiteral("protocol"), operation + QStringLiteral(": превышен безопасный размер ответа"), retryable);
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || doc.isNull())
        fail(service, phase, QStringLiteral("protocol"), operation + QStringLiteral(": некорректный JSON"), retryable);
    return doc;
}

void ensureHttp(const HttpResponse& response, const QString& service, const QString& phase,
                const QString& operation, const QString& expectedType, bool timeoutRetryable = true) {
    if (response.canceled) fail(service, phase, QStringLiteral("canceled"), operation + QStringLiteral(": операция отменена"));
    if (response.timedOut) fail(service, phase, QStringLiteral("timeout"), operation + QStringLiteral(": истекло время ожидания"), timeoutRetryable);
    if (response.oversized) fail(service, phase, QStringLiteral("protocol"), operation + QStringLiteral(": превышен безопасный размер ответа"), service == QStringLiteral("yandex") && phase == QStringLiteral("select"));
    if (response.status == 401 || response.status == 403) {
        fail(service, phase, QStringLiteral("auth"), operation + QStringLiteral(": HTTP статус %1").arg(response.status), true);
    }
    if (response.networkError == QNetworkReply::TimeoutError)
        fail(service, phase, QStringLiteral("timeout"), operation + QStringLiteral(": истекло время ожидания"), timeoutRetryable);
    if (response.status == 0 && response.networkError != QNetworkReply::NoError)
        fail(service, phase, QStringLiteral("unavailable"), operation + QStringLiteral(": сетевой запрос не завершён"), true);
    if (response.status < 200 || response.status >= 300) {
        const bool transientStatus = response.status == 408 || response.status == 429 || response.status >= 500;
        const QString code = transientStatus ? QStringLiteral("unavailable") : QStringLiteral("protocol");
        const bool retryable = code == QStringLiteral("unavailable");
        fail(service, phase, code, operation + QStringLiteral(": HTTP статус %1").arg(response.status), retryable);
    }
    if (response.networkError != QNetworkReply::NoError)
        fail(service, phase, QStringLiteral("unavailable"), operation + QStringLiteral(": сетевой запрос не завершён"), true);
    const auto media = response.contentType.split(';').first().trimmed().toLower();
    if (media != expectedType.toLatin1().toLower())
        fail(service, phase, QStringLiteral("protocol"), operation + QStringLiteral(": неверный Content-Type"), service == QStringLiteral("yandex") && phase == QStringLiteral("select"));
    const auto encoding = response.contentEncoding.trimmed().toLower();
    if (!encoding.isEmpty() && encoding != QByteArrayLiteral("identity"))
        fail(service, phase, QStringLiteral("protocol"), operation + QStringLiteral(": неожиданный Content-Encoding"), service == QStringLiteral("yandex") && phase == QStringLiteral("select"));
}

QString cacheBust(QString raw) {
    QUrl url(raw);
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("_"), QString::number(QDateTime::currentMSecsSinceEpoch()));
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded);
}

QString endpointHost(const QString& raw) { return QUrl(raw).host().toLower() + (QUrl(raw).port(-1) >= 0 ? QStringLiteral(":%1").arg(QUrl(raw).port()) : QString()); }

double medianOf(QList<double> values) {
    if (values.isEmpty()) return 0;
    std::sort(values.begin(), values.end());
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

double medianAbsoluteDeviation(const QList<double>& samples) {
    if (samples.size() < 2) return 0;
    const auto center = medianOf(samples);
    QList<double> deviations;
    for (const double sample : samples) deviations.append(std::abs(sample - center));
    return medianOf(deviations);
}

double qmsJitter(const QList<double>& samples, double median) {
    if (samples.size() < 2) return 0;
    auto ordered = samples;
    std::sort(ordered.begin(), ordered.end());
    const auto quarter = ordered.size() / 4;
    auto trimmed = ordered.mid(quarter, ordered.size() - 2 * quarter);
    if (trimmed.size() < 2) trimmed = ordered;
    double total = 0;
    for (qsizetype i = 1; i < trimmed.size(); ++i) total += std::abs(trimmed[i] - trimmed[i - 1]);
    double jitter = std::round(total / static_cast<double>(trimmed.size() - 1));
    if (jitter == 0) jitter = 1;
    if (jitter >= median) jitter = ordered.first();
    return jitter;
}

QString normalizeHost(QString host) {
    host = host.trimmed();
    if (host.isEmpty() || host.contains(QRegularExpression(QStringLiteral("[/;?#@\\s]"))) || host.contains(QStringLiteral("://"))) return {};
    QHostAddress address;
    const QString unbracketed = host.startsWith('[') && host.endsWith(']') ? host.mid(1, host.size() - 2) : host;
    if (address.setAddress(unbracketed)) return address.toString().contains(':') ? QStringLiteral("[%1]:20000").arg(address.toString()) : address.toString() + QStringLiteral(":20000");
    QString hostname;
    QString port = QStringLiteral("20000");
    if (host.startsWith('[')) {
        const auto close = host.indexOf(']');
        if (close <= 1) return {};
        hostname = host.mid(1, close - 1);
        address.clear();
        if (!address.setAddress(hostname) || address.protocol() != QAbstractSocket::IPv6Protocol) return {};
        if (close + 1 < host.size()) {
            if (host[close + 1] != ':') return {};
            port = host.mid(close + 2);
        }
        hostname = address.toString();
    } else {
        const auto colon = host.lastIndexOf(':');
        if (colon >= 0) {
            hostname = host.left(colon);
            port = host.mid(colon + 1);
            if (hostname.contains(':')) return {};
        } else hostname = host;
    }
    bool ok = false;
    const int portNumber = port.toInt(&ok);
    if (hostname.isEmpty() || !ok || portNumber < 1 || portNumber > 65535) return {};
    return hostname.contains(':') ? QStringLiteral("[%1]:%2").arg(hostname).arg(portNumber) : hostname + QStringLiteral(":%1").arg(portNumber);
}

bool sameOrigin(const QUrl& a, const QUrl& b) {
    const auto effectivePort = [](const QUrl& url) { return url.port(url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443 : 80); };
    return a.scheme().compare(b.scheme(), Qt::CaseInsensitive) == 0 &&
        a.host().compare(b.host(), Qt::CaseInsensitive) == 0 && effectivePort(a) == effectivePort(b);
}

bool validIp(QString value, QString* normalized = nullptr) {
    QHostAddress address;
    if (!address.setAddress(value) || !address.scopeId().isEmpty()) return false;
    if (address.protocol() == QAbstractSocket::IPv4Protocol || address.protocol() == QAbstractSocket::IPv6Protocol) {
        if (address.protocol() == QAbstractSocket::IPv6Protocol) {
            bool mapped = false;
            const auto ipv4 = address.toIPv4Address(&mapped);
            if (mapped) address = QHostAddress(ipv4);
        }
        if (normalized) *normalized = address.toString();
        return true;
    }
    return false;
}

struct YandexProbeSet {
    QStringList latency;
    QStringList download;
    QString host;
};

class YandexBackend final : public Backend {
public:
    explicit YandexBackend(BackendOptions options = {}) : options_(std::move(options)) {}
    QString id() const override { return QStringLiteral("yandex"); }

    QJsonObject select(const Config&, const Cancellation& cancellation) override {
        phaseClock_.start();
        try {
            requireActive(cancellation, QStringLiteral("select"));
            const auto probeUrl = options_.yandexBase.resolved(QUrl(QStringLiteral("/internet/api/v0/get-probes?flag_ws-conn-timeout=2000")));
            const auto response = perform(manager_, requestFor(QUrl(cacheBust(probeUrl.toString()))), maxYandexDiscovery, remaining(15000), cancellation, QStringLiteral("select"));
            ensureHttp(response, id(), QStringLiteral("select"), QStringLiteral("get-probes"), QStringLiteral("application/json"));
            const auto doc = parseJson(response.body, id(), QStringLiteral("select"), QStringLiteral("get-probes"), maxYandexDiscovery);
            if (!doc.isObject()) fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("get-probes вернул не объект"), true);
            const auto root = doc.object();
            if (root.value("mid").isString() == false || root.value("mid").toString().trimmed().isEmpty())
                fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("get-probes вернул пустой mid"), true);
            const auto lat = root.value("latency").toObject().value("probes");
            const auto down = root.value("download").toObject().value("probes");
            const auto up = root.value("upload").toObject().value("probes");
            if (!lat.isArray() || !down.isArray() || !up.isArray())
                fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("get-probes не содержит массивы probes"), true);
            YandexProbeSet set;
            QSet<QString> latHosts, downHosts, upHosts;
            for (const auto& value : lat.toArray()) {
                if (!value.isObject() || !value.toObject().value("url").isString()) fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("неверная latency-проба"), true);
                const QString raw = value.toObject().value("url").toString();
                checkedUrl(raw, QStringLiteral("https"), QStringLiteral("select"), options_.allowLoopbackHTTP);
                const auto host = endpointHost(raw);
                if (latHosts.contains(host)) fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("повторяющийся CDN latency-пробы"), true);
                latHosts.insert(host); set.latency.append(raw);
            }
            for (const auto& value : down.toArray()) {
                if (!value.isObject()) fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("неверная download-проба"), true);
                const auto item = value.toObject();
                const QString raw = item.value("url").toString();
                checkedUrl(raw, QStringLiteral("https"), QStringLiteral("select"), options_.allowLoopbackHTTP);
                const auto timeout = item.value("timeout");
                if (!timeout.isUndefined() && (!timeout.isDouble() || timeout.toDouble() != std::floor(timeout.toDouble())))
                    fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("неверное поле timeout download-пробы"), true);
                if (item.value("timeout").toInt(0) != 0) continue;
                if (!QUrl(raw).path().endsWith(QStringLiteral("/probes/50mb"))) fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("неожиданный URL основной download-пробы"), true);
                const auto host = endpointHost(raw);
                if (downHosts.contains(host)) fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("повторяющийся CDN download-пробы"), true);
                downHosts.insert(host); set.download.append(raw);
            }
            for (const auto& value : up.toArray()) {
                if (!value.isObject()) fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("неверная upload-проба"), true);
                const auto item = value.toObject();
                if (!item.value("size").isDouble() || item.value("size").toInt() <= 0)
                    fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("неверный размер upload-пробы"), true);
                const QString raw = item.value("url").toString(), post = item.value("postUrl").toString(), stats = item.value("statsUrl").toString();
                checkedUrl(raw, QStringLiteral("https"), QStringLiteral("select"), options_.allowLoopbackHTTP);
                checkedUrl(post, QStringLiteral("https"), QStringLiteral("select"), options_.allowLoopbackHTTP);
                checkedUrl(stats, QStringLiteral("https"), QStringLiteral("select"), options_.allowLoopbackHTTP);
                const auto host = endpointHost(raw);
                if (endpointHost(post) != host || endpointHost(stats) != host) fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("upload endpoints относятся к разным CDN"), true);
                const QString ws = item.value("websocketUrl").toString();
                if (!ws.isEmpty()) {
                    checkedUrl(ws, QStringLiteral("wss"), QStringLiteral("select"), options_.allowLoopbackHTTP);
                    if (endpointHost(ws) != host || item.value("websocketConnectionTimeout").toInt() <= 0 || item.value("websocketConnectionTimeout").toInt() > 30000)
                        fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("неверная WebSocket upload-проба"), true);
                }
                const auto timeout = item.value("timeout");
                if (!timeout.isUndefined() && (!timeout.isDouble() || timeout.toDouble() != std::floor(timeout.toDouble())))
                    fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("неверное поле timeout upload-пробы"), true);
                if (item.value("timeout").toInt(0) == 0) {
                    if (upHosts.contains(host)) fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("повторяющийся CDN upload-пробы"), true);
                    upHosts.insert(host);
                }
            }
            if (set.latency.isEmpty() || set.download.isEmpty() || upHosts.isEmpty() || latHosts != downHosts || latHosts != upHosts)
                fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("неполный или несогласованный набор CDN проб"), true);
            set.host = endpointHost(set.download.first());
            probes_ = set;
            QJsonObject server{{"name", set.host}, {"city", QJsonValue::Null}, {"region", QJsonValue::Null}};
            return server;
        } catch (const OpError&) { throw; }
    }

    QJsonObject ping(const Config&, const Cancellation& cancellation) override {
        if (probes_.latency.isEmpty()) fail(id(), QStringLiteral("ping"), QStringLiteral("internal"), QStringLiteral("сначала выберите сервер"));
        struct Worker {
            int attempts = 0;
            QNetworkReply* reply = nullptr;
            QElapsedTimer clock;
            qint64 headerNsecs = -1;
            QByteArray body;
            bool oversized = false;
            bool timedOut = false;
            int status = 0;
            QNetworkReply::NetworkError error = QNetworkReply::NoError;
            QList<double> samples;
            QString lastFailure;
        };
        std::vector<Worker> workers(static_cast<size_t>(probes_.latency.size()));
        QEventLoop loop;
        QTimer timer;
        timer.setInterval(10);
        QElapsedTimer phaseDeadline;
        phaseDeadline.start();
        int completed = 0;
        bool canceled = false;
        bool phaseTimedOut = false;
        std::function<void(int)> launch;
        launch = [&](int index) {
            auto& worker = workers[static_cast<size_t>(index)];
            worker.headerNsecs = -1;
            worker.body.clear();
            worker.oversized = false;
            worker.timedOut = false;
            worker.status = 0;
            worker.error = QNetworkReply::NoError;
            worker.clock.start();
            auto* reply = manager_.get(requestFor(QUrl(cacheBust(probes_.latency[index])), "*/*"));
            worker.reply = reply;
            reply->setReadBufferSize(maxPingBody + 1);
            QObject::connect(reply, &QNetworkReply::metaDataChanged, &loop, [&, index] {
                auto& current = workers[static_cast<size_t>(index)];
                if (current.headerNsecs < 0) current.headerNsecs = current.clock.nsecsElapsed();
            });
            QObject::connect(reply, &QIODevice::readyRead, &loop, [&, index, reply] {
                auto& current = workers[static_cast<size_t>(index)];
                current.body.append(reply->read(maxPingBody + 1 - current.body.size()));
                if (current.body.size() > maxPingBody) {
                    current.oversized = true;
                    reply->abort();
                }
            });
            QObject::connect(reply, &QNetworkReply::finished, &loop, [&, index, reply] {
                auto& current = workers[static_cast<size_t>(index)];
                current.body.append(reply->read(maxPingBody + 1 - current.body.size()));
                current.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                current.error = reply->error();
                const auto encoding = reply->rawHeader("Content-Encoding").trimmed().toLower();
                const bool valid = !current.timedOut && !current.oversized && current.status >= 200 && current.status < 300 &&
                    current.error == QNetworkReply::NoError && current.headerNsecs >= 0 && current.body.size() <= maxPingBody &&
                    (encoding.isEmpty() || encoding == QByteArrayLiteral("identity"));
                if (valid) current.samples.append(static_cast<double>(current.headerNsecs) / 1'000'000.0);
                else if (current.oversized || (!encoding.isEmpty() && encoding != QByteArrayLiteral("identity"))) current.lastFailure = QStringLiteral("protocol");
                else if (current.status == 401 || current.status == 403) current.lastFailure = QStringLiteral("auth");
                else if (current.timedOut || current.error == QNetworkReply::TimeoutError) current.lastFailure = QStringLiteral("timeout");
                else if (current.status > 0 && current.status < 500 && current.status != 408 && current.status != 429) current.lastFailure = QStringLiteral("protocol");
                else current.lastFailure = QStringLiteral("unavailable");
                current.reply = nullptr;
                reply->deleteLater();
                ++current.attempts;
                if (!canceled && !phaseTimedOut && !cancellation.canceled() && current.attempts < 4) launch(index);
                else if (++completed == static_cast<int>(workers.size())) loop.quit();
            });
        };
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
            if (cancellation.canceled()) {
                canceled = true;
                for (auto& worker : workers) if (worker.reply) worker.reply->abort();
                return;
            }
            if (phaseDeadline.elapsed() >= 15000) {
                phaseTimedOut = true;
                for (auto& worker : workers) if (worker.reply) { worker.timedOut = true; worker.reply->abort(); }
                return;
            }
            for (auto& worker : workers) {
                if (worker.reply && worker.clock.elapsed() >= 5000) {
                    worker.timedOut = true;
                    worker.reply->abort();
                }
            }
        });
        requireActive(cancellation, QStringLiteral("ping"));
        for (int i = 0; i < static_cast<int>(workers.size()); ++i) launch(i);
        timer.start();
        loop.exec();
        timer.stop();
        for (auto& worker : workers) {
            if (auto* reply = worker.reply) {
                worker.reply = nullptr;
                reply->abort();
                reply->deleteLater();
            }
        }
        if (canceled || cancellation.canceled()) fail(id(), QStringLiteral("ping"), QStringLiteral("canceled"), QStringLiteral("операция отменена"));
        if (phaseTimedOut) fail(id(), QStringLiteral("ping"), QStringLiteral("timeout"), QStringLiteral("истекло время выполнения ping"), true);
        QList<double> values;
        int bestIndex = -1;
        double best = std::numeric_limits<double>::infinity();
        QString failureCode = QStringLiteral("unavailable");
        for (int i = 0; i < static_cast<int>(workers.size()); ++i) {
            const auto& worker = workers[static_cast<size_t>(i)];
            values.append(worker.samples);
            for (const auto value : worker.samples) if (value < best) { best = value; bestIndex = i; }
            if (worker.lastFailure == QStringLiteral("auth")) failureCode = QStringLiteral("auth");
            else if (failureCode != QStringLiteral("auth") && worker.lastFailure == QStringLiteral("protocol")) failureCode = QStringLiteral("protocol");
            else if (failureCode != QStringLiteral("auth") && failureCode != QStringLiteral("protocol") && worker.lastFailure == QStringLiteral("timeout")) failureCode = QStringLiteral("timeout");
        }
        if (values.isEmpty()) fail(id(), QStringLiteral("ping"), failureCode, QStringLiteral("все запросы задержки завершились с ошибкой"), failureCode == QStringLiteral("auth") || failureCode == QStringLiteral("timeout") || failureCode == QStringLiteral("unavailable"));
        auto phase = pingPhase(values, QStringLiteral("minimum"), true);
        auto winningSamples = workers[static_cast<size_t>(bestIndex)].samples;
        if (winningSamples.size() > 1) winningSamples.removeFirst();
        phase.insert(QStringLiteral("jitter_ms"), medianAbsoluteDeviation(winningSamples));
        return phase;
    }

    QJsonObject download(const Config&, const Cancellation&) override { unavailable(QStringLiteral("download")); }
    QJsonObject upload(const Config&, const Cancellation&) override { unavailable(QStringLiteral("upload")); }

    QJsonObject connection(const Cancellation& cancellation) override {
        try {
            phaseClock_.start();
            const auto pageUrl = options_.yandexBase.resolved(QUrl(QStringLiteral("/internet/")));
            const auto response = perform(manager_, requestFor(pageUrl, "text/html"), maxYandexPage, remaining(15000), cancellation, QStringLiteral("connection"));
            ensureHttp(response, id(), QStringLiteral("connection"), QStringLiteral("страница интернетометра"), QStringLiteral("text/html"));
            const QByteArray marker = "Client.default(";
            int start = response.body.indexOf(marker);
            if (start < 0) fail(id(), QStringLiteral("connection"), QStringLiteral("protocol"), QStringLiteral("страница не содержит состояния клиента"));
            start += marker.size();
            while (start < response.body.size() && QByteArray(" \t\r\n").contains(response.body[start])) ++start;
            if (start >= response.body.size() || response.body[start] != '{') fail(id(), QStringLiteral("connection"), QStringLiteral("protocol"), QStringLiteral("состояние клиента не является JSON объектом"));
            int depth = 0; bool quoted = false, escaped = false; int end = -1;
            for (int i = start; i < response.body.size(); ++i) {
                const char c = response.body[i];
                if (quoted) { if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false; continue; }
                if (c == '"') quoted = true; else if (c == '{') ++depth; else if (c == '}' && --depth == 0) { end = i + 1; break; }
            }
            if (end < 0) fail(id(), QStringLiteral("connection"), QStringLiteral("protocol"), QStringLiteral("не удалось разобрать состояние клиента"));
            const auto doc = parseJson(response.body.mid(start, end - start), id(), QStringLiteral("connection"), QStringLiteral("state"), maxYandexPage, false);
            const auto ip = doc.object().value("ip").toObject();
            QString parsed;
            if (!validIp(ip.value("v4").toString(), &parsed) && !validIp(ip.value("v6").toString(), &parsed))
                fail(id(), QStringLiteral("connection"), QStringLiteral("protocol"), QStringLiteral("страница не сообщила действительный IP-адрес"));
            return QJsonObject{{"status", "ok"}, {"external_ip", parsed}, {"isp", QJsonValue::Null}, {"detected_by", id()}, {"warnings", QJsonArray{}}, {"error", QJsonValue::Null}};
        } catch (const OpError&) {
            throw;
        }
    }
private:
    int remaining(int requested) const { return std::max(1, requested - static_cast<int>(phaseClock_.elapsed())); }
    [[noreturn]] void unavailable(const QString& phase) {
        fail(id(), phase, QStringLiteral("unavailable"), QStringLiteral("пропускная способность ещё не перенесена в C++ backend"));
    }
    QNetworkAccessManager manager_;
    BackendOptions options_;
    YandexProbeSet probes_;
    QElapsedTimer phaseClock_;
};

struct QmsServer { QString host, name, city, region; double median = 0; QList<double> samples; };

class SpeedtestBackend final : public Backend {
public:
    explicit SpeedtestBackend(BackendOptions options = {}) : options_(std::move(options)) {}
    QString id() const override { return QStringLiteral("speedtest"); }
    QJsonObject select(const Config& config, const Cancellation& cancellation) override {
        try {
            phaseClock_.start();
            requireActive(cancellation, QStringLiteral("select"));
            if (!config.server.isEmpty()) {
                const QString host = normalizeHost(config.server);
                if (host.isEmpty()) fail(id(), QStringLiteral("select"), QStringLiteral("internal"), QStringLiteral("неверный адрес сервера"));
                const QUrl parsed(QStringLiteral("wss://") + host + QStringLiteral("/"), QUrl::StrictMode);
                if (!parsed.isValid() || parsed.host().isEmpty() || parsed.port(-1) < 1 || parsed.port(-1) > 65535 || !parsed.userInfo().isEmpty())
                    fail(id(), QStringLiteral("select"), QStringLiteral("internal"), QStringLiteral("неверный адрес сервера"));
                selected_ = QmsServer{host, host, {}, {}, 0, {}};
                return serverJson(selected_);
            }
            const auto url = options_.speedtestBase.resolved(QUrl(QStringLiteral("/api/nearest_servers?t=") + QString::number(QDateTime::currentMSecsSinceEpoch())));
            auto key = browserKey_;
            auto request = requestFor(url);
            request.setRawHeader("x-api-key", key.toUtf8());
            auto response = perform(manager_, request, maxSpeedDiscovery, remaining(15000), cancellation, QStringLiteral("select"));
            if (response.status == 401 || response.status == 403) {
                key = refreshKey(cancellation, QStringLiteral("select"));
                request = requestFor(url);
                request.setRawHeader("x-api-key", key.toUtf8());
                response = perform(manager_, request, maxSpeedDiscovery, remaining(15000), cancellation, QStringLiteral("select"));
            }
            ensureHttp(response, id(), QStringLiteral("select"), QStringLiteral("nearest_servers"), QStringLiteral("application/json"));
            const auto doc = parseJson(response.body, id(), QStringLiteral("select"), QStringLiteral("nearest_servers"), maxSpeedDiscovery);
            const auto root = doc.object();
            if (root.value("algorithm").toString().trimmed().isEmpty() || !root.value("data").isArray())
                fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("nearest_servers вернул неверную схему"), true);
            QList<QmsServer> candidates; QSet<QString> seen;
            for (const auto& entry : root.value("data").toArray()) {
                if (!entry.isObject()) continue;
                const auto row = entry.toObject();
                const QUrl src(row.value("src").toString(), QUrl::StrictMode);
                const int port = row.value("port").toInt();
                const int serverId = row.value("id").toInt();
                const QString name = row.value("name").toString().trimmed(), city = row.value("city").toString().trimmed(), source = row.value("source").toString().trimmed();
                if (!src.isValid() || src.host().isEmpty() || !checkedSourceAllowed(src) || !src.userInfo().isEmpty() || !src.query().isEmpty() || !src.fragment().isEmpty() || (!src.path().isEmpty() && src.path() != QStringLiteral("/")) || serverId <= 0 || name.isEmpty() || city.isEmpty() || source.isEmpty() || port < 1 || port > 65535) continue;
                const QString sourceHost = src.host();
                const QString host = (sourceHost.contains(':') ? QStringLiteral("[%1]").arg(sourceHost) : sourceHost) + QStringLiteral(":%1").arg(port);
                if (seen.contains(host)) continue;
                seen.insert(host); candidates.append({host, name, city, row.value("region_name").toString(), 0, {}});
            }
            if (candidates.isEmpty()) fail(id(), QStringLiteral("select"), QStringLiteral("protocol"), QStringLiteral("nearest_servers не вернул корректных адресов"), true);
            if (candidates.size() > 8) candidates = candidates.mid(0, 8);
            QList<QmsServer> responsive = probeCandidates(candidates, cancellation);
            if (responsive.isEmpty()) fail(id(), QStringLiteral("select"), QStringLiteral("unavailable"), QStringLiteral("ни один найденный сервер QMS не завершил согласование WebSocket"), true);
            std::stable_sort(responsive.begin(), responsive.end(), [](const auto& a, const auto& b) { return a.median < b.median; });
            selected_ = responsive.first();
            candidates_ = responsive.mid(0, static_cast<int>(std::min<qsizetype>(4, responsive.size())));
            return serverJson(selected_);
        } catch (const OpError&) { throw; }
    }
    QJsonObject ping(const Config&, const Cancellation& cancellation) override {
        phaseClock_.start();
        try {
            if (selected_.host.isEmpty()) fail(id(), QStringLiteral("ping"), QStringLiteral("internal"), QStringLiteral("сначала выберите сервер"));
            QList<QmsServer> ordered{selected_};
            for (const auto& candidate : candidates_) if (candidate.host != selected_.host) ordered.append(candidate);
            QString lastCode = QStringLiteral("unavailable");
            for (auto candidate : ordered) {
                requireActive(cancellation, QStringLiteral("ping"));
                if (candidate.host == selected_.host && candidate.samples.size() == 10) {
                    selected_.samples = candidate.samples;
                    auto phase = pingPhase(candidate.samples, QStringLiteral("median"));
                    phase.insert(QStringLiteral("jitter_ms"), qmsJitter(candidate.samples, median(candidate.samples)));
                    return phase;
                }
                QList<double> samples;
                QString failureCode;
                if (probe(candidate.host, 10, cancellation, &samples, QStringLiteral("ping"), &failureCode)) {
                    candidate.samples = samples;
                    candidate.median = median(samples);
                    selected_ = candidate;
                    auto phase = pingPhase(samples, QStringLiteral("median"));
                    phase.insert(QStringLiteral("jitter_ms"), qmsJitter(samples, median(samples)));
                    return phase;
                }
                if (!failureCode.isEmpty()) lastCode = failureCode;
            }
            fail(id(), QStringLiteral("ping"), lastCode, QStringLiteral("QMS ping завершился с ошибкой"), lastCode != QStringLiteral("canceled") && lastCode != QStringLiteral("protocol"));
        } catch (const OpError&) { throw; }
    }
    QJsonObject download(const Config&, const Cancellation&) override { unavailable(QStringLiteral("download")); }
    QJsonObject upload(const Config&, const Cancellation&) override { unavailable(QStringLiteral("upload")); }
    QJsonObject connection(const Cancellation& cancellation) override {
        try {
            phaseClock_.start();
            QString key = browserKey_;
            auto fetch = [&](const QString& path, const QString& currentKey) {
                auto req = requestFor(options_.speedtestBase.resolved(QUrl(path + QStringLiteral("?t=") + QString::number(QDateTime::currentMSecsSinceEpoch()))));
                req.setRawHeader("x-api-key", currentKey.toUtf8());
                return perform(manager_, req, maxJSONConnection, remaining(15000), cancellation, QStringLiteral("connection"));
            };
            auto response = fetch(QStringLiteral("/api/asn_provider/ip"), key);
            if (response.status == 401 || response.status == 403) { key = refreshKey(cancellation, QStringLiteral("connection")); response = fetch(QStringLiteral("/api/asn_provider/ip"), key); }
            ensureHttp(response, id(), QStringLiteral("connection"), QStringLiteral("asn_provider/ip"), QStringLiteral("application/json"));
            auto ipJson = parseJson(response.body, id(), QStringLiteral("connection"), QStringLiteral("asn_provider/ip"), maxJSONConnection, false).object();
            QString ip;
            if (!validIp(ipJson.value("ip").toString().trimmed(), &ip)) fail(id(), QStringLiteral("connection"), QStringLiteral("protocol"), QStringLiteral("speedtest.ru вернул неверный IP-адрес"));
            QJsonArray warnings;
            QString isp;
            try {
                auto asn = fetch(QStringLiteral("/api/asn_provider/asn"), key);
                if (asn.status == 401 || asn.status == 403) {
                    key = refreshKey(cancellation, QStringLiteral("connection"));
                    asn = fetch(QStringLiteral("/api/asn_provider/asn"), key);
                }
                ensureHttp(asn, id(), QStringLiteral("connection"), QStringLiteral("asn_provider/asn"), QStringLiteral("application/json"));
                const auto value = parseJson(asn.body, id(), QStringLiteral("connection"), QStringLiteral("asn_provider/asn"), maxJSONConnection, false).object();
                QString asnIp;
                if (validIp(value.value("ip").toString().trimmed(), &asnIp) && asnIp == ip) {
                    isp = value.value("provider_name").toString().trimmed();
                    if (isp.toUtf8().size() > 255 || isp.contains(QRegularExpression(QStringLiteral("[\\r\\n\\x00]")))) isp.clear();
                }
            } catch (const OpError& error) {
                if (error.code == QStringLiteral("canceled") || error.code == QStringLiteral("timeout") || remaining(15000) <= 0) throw;
                // Provider metadata is best effort; a valid IP remains usable.
            }
            if (isp.isEmpty()) warnings.append(QStringLiteral("speedtest.ru не сообщил корректное название интернет-провайдера"));
            return QJsonObject{{"status", "ok"}, {"external_ip", ip}, {"isp", isp.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(isp)}, {"detected_by", id()}, {"warnings", warnings}, {"error", QJsonValue::Null}};
        } catch (const OpError&) {
            throw;
        }
    }
private:
    int remaining(int requested) const { return phaseClock_.isValid() ? requested - static_cast<int>(phaseClock_.elapsed()) : requested; }
    static double median(QList<double> values) {
        return medianOf(std::move(values));
    }
    static QJsonObject serverJson(const QmsServer& server) {
        return {{"name", server.host}, {"city", server.city.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(server.city)}, {"region", server.region.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(server.region)}};
    }
    [[noreturn]] void unavailable(const QString& phase) {
        fail(id(), phase, QStringLiteral("unavailable"), QStringLiteral("пропускная способность ещё не перенесена в C++ backend"));
    }
    bool probe(const QString& host, int count, const Cancellation& cancellation, QList<double>* samples,
               const QString& phase, QString* failureCode = nullptr,
               const std::chrono::steady_clock::time_point* sharedDeadline = nullptr) {
        const auto timeLeft = [&] {
            if (!sharedDeadline) return remaining(15000);
            return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(*sharedDeadline - std::chrono::steady_clock::now()).count());
        };
        if (timeLeft() <= 0) fail(id(), phase, QStringLiteral("timeout"), QStringLiteral("истекло время выполнения измерения"), true);
        QWebSocket socket;
        socket.setReadBufferSize(maxWebSocketMessage);
        socket.setMaxAllowedIncomingFrameSize(maxWebSocketMessage);
        socket.setMaxAllowedIncomingMessageSize(maxWebSocketMessage);
        QEventLoop loop; QTimer timer; timer.setInterval(10); QElapsedTimer deadline;
        const int probeTimeout = std::min(8000, timeLeft());
        bool done = false, ok = false; QString failure; int state = 0, receivedPings = 0;
        QElapsedTimer pingClock;
        auto finishError = [&](const QString& message, const QString& code = QStringLiteral("protocol")) { if (done) return; failure = message; if (failureCode) *failureCode = code; done = true; socket.abort(); loop.quit(); };
        auto send = [&](const QString& message) { if (socket.sendTextMessage(message) < 0) finishError(QStringLiteral("ошибка отправки QMS команды"), QStringLiteral("unavailable")); };
        QObject::connect(&socket, &QWebSocket::connected, &loop, [&] { state = 1; send(QStringLiteral("HI")); });
        QObject::connect(&socket, &QWebSocket::textMessageReceived, &loop, [&](const QString& message) {
            if (done) return;
            if (message.toUtf8().size() > maxWebSocketMessage) { finishError(QStringLiteral("слишком большой QMS ответ")); return; }
            if (state == 1) {
                if (message.trimmed() != QStringLiteral("HELLO")) { finishError(QStringLiteral("неожиданный QMS HELLO")); return; }
                state = 2; send(QStringLiteral("GETINFO")); return;
            }
            if (state == 2) {
                const auto normalized = message.trimmed().toLower();
                bool valid = normalized == QStringLiteral("qms_testing") || normalized.startsWith(QStringLiteral("qms_testing/"));
                if (!valid) {
                    const auto info = QJsonDocument::fromJson(message.toUtf8()).object();
                    valid = info.value("name").toString().compare(QStringLiteral("qms_testing"), Qt::CaseInsensitive) == 0;
                }
                if (!valid) { finishError(QStringLiteral("неожиданная информация QMS")); return; }
                state = 3; send(QStringLiteral("AUTH")); return;
            }
            if (state == 3) {
                if (message.trimmed() != QStringLiteral("READY_TO_TEST")) { finishError(QStringLiteral("неожиданный QMS AUTH ответ")); return; }
                state = 4; pingClock.start(); send(QStringLiteral("PING")); return;
            }
            if (state == 4) {
                const auto fields = message.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
                bool timestampOk = false;
                if (fields.size() != 2 || fields[0] != QStringLiteral("PONG") || fields[1].toLongLong(&timestampOk) < 0 || !timestampOk) { finishError(QStringLiteral("неверный QMS PONG")); return; }
                samples->append(static_cast<double>(pingClock.nsecsElapsed()) / 1'000'000.0);
                ++receivedPings;
                if (receivedPings >= count) { ok = true; done = true; loop.quit(); return; }
                pingClock.restart(); send(QStringLiteral("PING"));
            }
        });
        QObject::connect(&socket, &QWebSocket::binaryMessageReceived, &loop, [&](const QByteArray&) { finishError(QStringLiteral("неожиданный binary кадр QMS")); });
        QObject::connect(&socket, &QWebSocket::errorOccurred, &loop, [&](QAbstractSocket::SocketError) { finishError(QStringLiteral("ошибка WebSocket QMS"), QStringLiteral("unavailable")); });
        QObject::connect(&socket, &QWebSocket::disconnected, &loop, [&] { if (!done) finishError(QStringLiteral("соединение WebSocket закрыто"), QStringLiteral("unavailable")); });
        QObject::connect(&timer, &QTimer::timeout, &loop, [&] {
            if (cancellation.canceled()) finishError(QStringLiteral("canceled"), QStringLiteral("canceled"));
            else if (timeLeft() <= 0 || deadline.elapsed() >= probeTimeout) finishError(QStringLiteral("timeout"), QStringLiteral("timeout"));
        });
        const QUrl hostUrl(QStringLiteral("//") + host);
        const QString scheme = options_.allowLoopbackHTTP && isLoopback(hostUrl.host()) ? QStringLiteral("ws://") : QStringLiteral("wss://");
        QNetworkRequest request(QUrl(scheme + host + QStringLiteral("/")));
        request.setRawHeader("User-Agent", userAgent);
        deadline.start(); timer.start(); socket.open(request); loop.exec(); timer.stop(); socket.abort();
        if (failure == QStringLiteral("canceled")) fail(id(), phase, QStringLiteral("canceled"), QStringLiteral("операция отменена"));
        return ok;
    }
    QList<QmsServer> probeCandidates(const QList<QmsServer>& candidates, const Cancellation& cancellation) {
        using Clock = std::chrono::steady_clock;
        struct ProbeResult { QList<double> samples; bool responsive = false; QString code; };

        const auto phaseLeft = remaining(15000);
        if (phaseLeft <= 0) fail(id(), QStringLiteral("select"), QStringLiteral("timeout"), QStringLiteral("истекло время выполнения измерения"), true);
        const auto started = Clock::now();
        const auto probeDeadline = started + std::chrono::milliseconds(std::min(8000, phaseLeft));
        std::vector<ProbeResult> results(static_cast<size_t>(candidates.size()));
        std::vector<QThread*> threads;
        threads.reserve(static_cast<size_t>(candidates.size()));
        std::mutex resultsMutex;
        QEventLoop dispatchLoop;
        QTimer cancellationTimer;
        cancellationTimer.setInterval(10);
        qsizetype next = 0;
        qsizetype active = 0;
        bool canceled = false;

        auto startNext = [&] {
            if (cancellation.canceled()) canceled = true;
            while (!canceled && next < candidates.size() && active < 4 &&
                   Clock::now() < probeDeadline && remaining(15000) > 0) {
                const qsizetype index = next++;
                const QString host = candidates[index].host;
                ++active;
                QThread* thread = QThread::create([&, index, host, probeDeadline] {
                    ProbeResult result;
                    try {
                        result.responsive = probe(host, 10, cancellation, &result.samples,
                                                  QStringLiteral("select"), &result.code, &probeDeadline);
                    } catch (const OpError& error) {
                        result.code = error.code;
                    } catch (...) {
                        result.code = QStringLiteral("unavailable");
                    }
                    {
                        std::lock_guard<std::mutex> lock(resultsMutex);
                        results[static_cast<size_t>(index)] = std::move(result);
                    }
                    QMetaObject::invokeMethod(&dispatchLoop, [&, index] {
                        --active;
                        startNext();
                        if ((canceled || next >= candidates.size() || Clock::now() >= probeDeadline || remaining(15000) <= 0) && active == 0)
                            dispatchLoop.quit();
                    }, Qt::QueuedConnection);
                });
                threads.push_back(thread);
                thread->start();
            }
            if ((canceled || next >= candidates.size() || Clock::now() >= probeDeadline || remaining(15000) <= 0) && active == 0)
                dispatchLoop.quit();
        };
        QObject::connect(&cancellationTimer, &QTimer::timeout, &dispatchLoop, [&] {
            if (cancellation.canceled()) canceled = true;
            if ((canceled || Clock::now() >= probeDeadline || remaining(15000) <= 0) && active == 0)
                dispatchLoop.quit();
        });
        cancellationTimer.start();
        startNext();
        if (active > 0 || next < candidates.size()) dispatchLoop.exec();
        cancellationTimer.stop();
        for (QThread* thread : threads) {
            thread->wait();
            delete thread;
        }
        if (canceled || cancellation.canceled()) fail(id(), QStringLiteral("select"), QStringLiteral("canceled"), QStringLiteral("операция отменена"));

        QList<QmsServer> responsive;
        for (qsizetype i = 0; i < next; ++i) {
            ProbeResult result;
            {
                std::lock_guard<std::mutex> lock(resultsMutex);
                result = results[static_cast<size_t>(i)];
            }
            if (!result.responsive) continue;
            auto candidate = candidates[i];
            candidate.samples = std::move(result.samples);
            candidate.median = median(candidate.samples);
            responsive.append(std::move(candidate));
        }
        return responsive;
    }
    QString refreshKey(const Cancellation& cancellation, const QString& phase) {
        if (keyRefreshAttempted_) fail(id(), phase, QStringLiteral("auth"), QStringLiteral("browser key уже обновлялся"));
        keyRefreshAttempted_ = true;
        auto page = perform(manager_, requestFor(options_.speedtestBase, "text/html"), maxYandexPage, remaining(15000), cancellation, phase);
        ensureHttp(page, id(), phase, QStringLiteral("страница speedtest.ru"), QStringLiteral("text/html"));
        const QRegularExpression assets(QStringLiteral("src=[\\\"']([^\\\"']+\\.js[^\\\"']*)[\\\"']"));
        const auto matches = assets.globalMatch(QString::fromUtf8(page.body));
        int count = 0;
        const QUrl origin = options_.speedtestBase;
        QRegularExpression keyPattern(QStringLiteral("QMSLibrary\\(\\{\\s*id\\s*:\\s*[\\\"']([A-Za-z0-9_-]{16,128})[\\\"']"));
        auto it = matches;
        while (it.hasNext() && count++ < 32) {
            const auto match = it.next();
            const auto url = origin.resolved(QUrl(match.captured(1)));
            if (!sameOrigin(origin, url)) continue;
            auto asset = perform(manager_, requestFor(url, "*/*"), maxYandexPage, remaining(10000), cancellation, phase);
            const auto media = asset.contentType.split(';').first().trimmed().toLower();
            const auto encoding = asset.contentEncoding.trimmed().toLower();
            if (asset.status < 200 || asset.status >= 300 || asset.canceled || asset.timedOut || asset.oversized || asset.networkError != QNetworkReply::NoError ||
                (!encoding.isEmpty() && encoding != QByteArrayLiteral("identity")) ||
                (media != QByteArrayLiteral("application/javascript") && media != QByteArrayLiteral("text/javascript") && media != QByteArrayLiteral("text/plain"))) continue;
            const auto found = keyPattern.match(QString::fromUtf8(asset.body));
            if (found.hasMatch()) { browserKey_ = found.captured(1); return browserKey_; }
        }
        fail(id(), phase, QStringLiteral("auth"), QStringLiteral("не удалось обновить browser key"), true);
    }
    bool checkedSourceAllowed(const QUrl& src) const {
        return src.scheme() == QStringLiteral("https") ||
            (options_.allowLoopbackHTTP && src.scheme() == QStringLiteral("http") && isLoopback(src.host()));
    }
    QNetworkAccessManager manager_;
    BackendOptions options_;
    QString browserKey_ = QString::fromLatin1(browserKey);
    bool keyRefreshAttempted_ = false;
    QmsServer selected_;
    QList<QmsServer> candidates_;
    QElapsedTimer phaseClock_;
};
} // namespace

std::unique_ptr<Backend> makeBackend(const QString& service) {
    if (service == QStringLiteral("yandex")) return std::make_unique<YandexBackend>();
    if (service == QStringLiteral("speedtest")) return std::make_unique<SpeedtestBackend>();
    throw std::invalid_argument("unknown service");
}

std::unique_ptr<Backend> makeBackendForTesting(const QString& service, const BackendOptions& options) {
    const QUrl base = service == QStringLiteral("yandex") ? options.yandexBase : options.speedtestBase;
    if (!options.allowLoopbackHTTP || !base.isValid() || !isLoopback(base.host()) ||
        (base.scheme() != QStringLiteral("http") && base.scheme() != QStringLiteral("https")) ||
        !base.userInfo().isEmpty() || !base.query().isEmpty() || !base.fragment().isEmpty())
        throw std::invalid_argument("test endpoints must be loopback and explicitly enabled");
    if (service == QStringLiteral("yandex")) return std::make_unique<YandexBackend>(options);
    if (service == QStringLiteral("speedtest")) return std::make_unique<SpeedtestBackend>(options);
    throw std::invalid_argument("unknown service");
}

} // namespace puls
