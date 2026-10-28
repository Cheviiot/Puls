#include "../src/yandex_throughput_p.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QMap>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketServer>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <optional>

using namespace std::chrono_literals;

namespace {

int failures = 0;
void expect(bool condition, const char* label) {
    if (!condition) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

void client(const std::function<void()>& fn) {
    QEventLoop loop;
    std::exception_ptr error;
    auto* thread = QThread::create([&] {
        try { fn(); } catch (...) { error = std::current_exception(); }
    });
    QObject::connect(thread, &QThread::finished, &loop, &QEventLoop::quit);
    thread->start();
    loop.exec();
    thread->wait();
    delete thread;
    if (error) std::rethrow_exception(error);
}

template<class Fn>
void expectError(Fn&& fn, const QString& code, const char* label) {
    try { client(fn); expect(false, label); }
    catch (const puls::OpError& error) {
        if (error.code != code) std::cerr << "Expected code " << code.toStdString() << ", got " << error.code.toStdString() << '\n';
        expect(error.code == code, label);
    }
    catch (...) { expect(false, label); }
}

struct WorkerState {
    std::atomic_bool stop{false};
    std::atomic_bool ready{false};
    std::atomic<qint64> bytes{0};
    bool stopOnRecord = false;
    puls::transfer::WorkerContext context() {
        return {
            [&] { return stop.load(); },
            [&] { ready.store(true); },
            [&](qint64 n) { bytes.fetch_add(n); if (stopOnRecord) stop.store(true); }
        };
    }
};

struct Request {
    QByteArray method;
    QByteArray target;
    QMap<QByteArray, QByteArray> headers;
    qint64 bodySize = 0;
};

class HttpServer {
public:
    using Handler = std::function<void(QTcpSocket*, const Request&)>;
    explicit HttpServer(Handler handler) : handler_(std::move(handler)) {
        if (!server_.listen(QHostAddress::LocalHost, 0)) throw std::runtime_error("listen failed");
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [&] {
            while (server_.hasPendingConnections()) {
                auto* socket = server_.nextPendingConnection();
                socket->setParent(&server_);
                auto buffer = std::make_shared<QByteArray>();
                auto request = std::make_shared<Request>();
                auto headerDone = std::make_shared<bool>(false);
                auto handled = std::make_shared<bool>(false);
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [=, this] {
                    if (*handled) { socket->readAll(); return; }
                    buffer->append(socket->readAll());
                    if (!*headerDone) {
                        const auto end = buffer->indexOf("\r\n\r\n");
                        if (end < 0) return;
                        const auto lines = buffer->left(end).split('\n');
                        const auto first = lines.first().trimmed().split(' ');
                        if (first.size() < 2) return;
                        request->method = first[0];
                        request->target = first[1];
                        for (qsizetype i = 1; i < lines.size(); ++i) {
                            const auto colon = lines[i].indexOf(':');
                            if (colon > 0) request->headers.insert(lines[i].left(colon).trimmed().toLower(), lines[i].mid(colon + 1).trimmed());
                        }
                        buffer->remove(0, end + 4);
                        *headerDone = true;
                    }
                    request->bodySize += buffer->size();
                    buffer->clear();
                    const auto expected = request->headers.value("content-length").toLongLong();
                    if (request->bodySize >= expected) {
                        *handled = true;
                        handler_(socket, *request);
                    }
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl url(const QString& path = QStringLiteral("/")) const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server_.serverPort()).arg(path));
    }
private:
    QTcpServer server_;
    Handler handler_;
};

void respond(QTcpSocket* socket, int status, QByteArray headers = {}, QByteArray body = {}) {
    socket->write("HTTP/1.1 " + QByteArray::number(status) + " Test\r\nConnection: close\r\n" + headers + "\r\n" + body);
    socket->disconnectFromHost();
}

puls::BackendOptions options(const QUrl& base) {
    puls::BackendOptions value;
    value.yandexBase = base;
    value.allowLoopbackHTTP = true;
    value.transferDuration = 70ms;
    value.transferWarmup = 0ms;
    value.transferReadyGrace = 0ms;
    value.transferStartupTimeout = 700ms;
    return value;
}

class WebSocketServer {
public:
    using Handler = std::function<void(QWebSocket*)>;
    explicit WebSocketServer(Handler handler)
        : server_(QStringLiteral("Yandex mock"), QWebSocketServer::NonSecureMode) {
        if (!server_.listen(QHostAddress::LocalHost, 0)) throw std::runtime_error("WS listen failed");
        QObject::connect(&server_, &QWebSocketServer::newConnection, &server_, [&, handler] {
            while (server_.hasPendingConnections()) {
                auto* socket = server_.nextPendingConnection();
                socket->setParent(&server_);
                ++connections;
                handler(socket);
                QObject::connect(socket, &QWebSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl url() const { return QUrl(QStringLiteral("ws://127.0.0.1:%1/upload").arg(server_.serverPort())); }
    int connections = 0;
private:
    QWebSocketServer server_;
};

void testDownloadConfirmedBytes() {
    bool requestValid = false;
    HttpServer server([&](QTcpSocket* socket, const Request& request) {
        requestValid = request.method == "GET" && request.target.contains("cb=") &&
            request.headers.value("accept-encoding") == "identity";
        respond(socket, 200, "Content-Type: application/octet-stream\r\nContent-Length: 4096\r\n", QByteArray(4096, 'x'));
    });
    WorkerState state;
    const auto opts = options(server.url());
    bool complete = false;
    client([&] {
        QNetworkAccessManager manager;
        complete = puls::yandex_transfer::downloadProbe(manager, server.url(), opts, state.context());
    });
    expect(complete && state.ready && state.bytes == 4096, "download counts exact successful HTTP bytes");
    expect(requestValid, "download uses GET, cache busting and identity encoding");
}

void testDownloadRejectsInvalidResponses() {
    struct Case { int status; QByteArray headers; QByteArray body; QString code; QString path; };
    const QList<Case> cases{
        {200, "Content-Type: text/html\r\nContent-Length: 4\r\n", "html", "protocol", "/"},
        {200, "Content-Type: application/octet-stream\r\nContent-Length: 4\r\nContent-Encoding: gzip\r\n", "xxxx", "protocol", "/"},
        {200, "Content-Type: application/octet-stream\r\n", "xxxx", "protocol", "/"},
        {200, "Content-Type: application/octet-stream\r\nContent-Length: 0\r\n", {}, "protocol", "/"},
        {200, "Content-Type: application/octet-stream\r\nContent-Length: 1073741825\r\n", {}, "protocol", "/"},
        {200, "Content-Type: application/octet-stream\r\nContent-Length: 4\r\n", "xxxx", "protocol", "/probes/50mb"},
        {401, "Content-Length: 0\r\n", {}, "auth", "/"},
        {403, "Content-Length: 0\r\n", {}, "auth", "/"},
        {500, "Content-Length: 0\r\n", {}, "unavailable", "/"},
        {302, "Location: /elsewhere\r\nContent-Length: 0\r\n", {}, "protocol", "/"},
    };
    for (const auto& test : cases) {
        HttpServer server([&](QTcpSocket* socket, const Request&) { respond(socket, test.status, test.headers, test.body); });
        WorkerState state;
        const auto opts = options(server.url());
        expectError([&] {
            QNetworkAccessManager manager;
            puls::yandex_transfer::downloadProbe(manager, server.url(test.path), opts, state.context());
        }, test.code, "download rejects invalid status, size, type and encoding");
        expect(!state.ready && state.bytes == 0, "invalid download response is never ready or counted");
    }
    HttpServer server([](QTcpSocket* socket, const Request&) {
        respond(socket, 200, "Content-Type: application/octet-stream\r\nContent-Length: 100\r\n", "short");
    });
    WorkerState state;
    const auto opts = options(server.url());
    expectError([&] {
        QNetworkAccessManager manager;
        puls::yandex_transfer::downloadProbe(manager, server.url(), opts, state.context());
    }, "unavailable", "download rejects truncated successful response");
    expect(state.bytes <= 5, "truncated response cannot invent bytes");
}

void testHTTPUploadConfirmedBlocks() {
    qint64 uploaded = 0;
    bool requestValid = false;
    HttpServer server([&](QTcpSocket* socket, const Request& request) {
        uploaded += request.bodySize;
        requestValid = request.method == "POST" && request.headers.value("content-type") == "application/octet-stream";
        respond(socket, 204, "Content-Length: 0\r\n");
    });
    WorkerState state;
    state.stopOnRecord = true;
    const auto opts = options(server.url());
    client([&] {
        QNetworkAccessManager manager;
        puls::yandex_transfer::httpUpload(manager, server.url(), opts, state.context());
    });
    expect(state.ready && state.bytes == (2 << 20) && uploaded == (2 << 20), "HTTP upload counts one confirmed 2 MiB block");
    expect(requestValid, "HTTP upload uses binary POST");
}

void testHTTPUploadRejectsInvalidResponses() {
    struct Case { int status; QByteArray headers; QByteArray body; QString code; };
    const QList<Case> cases{
        {500, "Content-Length: 0\r\n", {}, "unavailable"},
        {403, "Content-Length: 0\r\n", {}, "auth"},
        {200, "Content-Length: 1048577\r\n", {}, "protocol"},
        {200, "Content-Length: 2\r\nContent-Encoding: gzip\r\n", "ok", "protocol"},
        {200, "Content-Length: 100\r\n", "short", "unavailable"},
        {200, {}, QByteArray((1 << 20) + 1, 'x'), "protocol"},
    };
    for (const auto& test : cases) {
        HttpServer server([&](QTcpSocket* socket, const Request&) { respond(socket, test.status, test.headers, test.body); });
        WorkerState state;
        const auto opts = options(server.url());
        expectError([&] {
            QNetworkAccessManager manager;
            puls::yandex_transfer::httpUpload(manager, server.url(), opts, state.context());
        }, test.code, "HTTP upload rejects failed or oversized acknowledgement");
        expect(!state.ready && state.bytes == 0, "HTTP upload never counts unconfirmed payload");
    }
}

void testWebSocketCountsOnlyACK() {
    bool queryValid = false;
    bool frameValid = true;
    int frames = 0;
    WebSocketServer server([&](QWebSocket* socket) {
        const QUrlQuery query(socket->requestUrl());
        queryValid = query.queryItemValue("type") == "upload" && query.queryItemValue("duration") == "4";
        QObject::connect(socket, &QWebSocket::binaryMessageReceived, socket, [&, socket](const QByteArray& frame) {
            frameValid = frameValid && frame.size() == (64 << 10);
            if (++frames == 1) {
                socket->sendTextMessage(QStringLiteral("{\"k\":\"u\",\"b\":0,\"i\":0}"));
                socket->sendTextMessage(QStringLiteral("{\"k\":\"u\",\"b\":137,\"i\":1}"));
            }
        });
    });
    WorkerState state;
    state.stopOnRecord = true;
    puls::YandexUploadProbe probe;
    probe.websocketUrl = server.url();
    const auto opts = options(QUrl("http://127.0.0.1"));
    client([&] { puls::yandex_transfer::websocketUpload(probe, opts, 3760ms, state.context()); });
    expect(state.ready && state.bytes == 137, "WebSocket counts only exact ACK delta after zero control ACK");
    expect(queryValid && frameValid && frames > 0, "WebSocket sends native query and 64 KiB binary frames");
}

void testWebSocketRejectsInvalidACKs() {
    struct Case { QByteArray payload; bool binary = false; };
    const QList<Case> cases{
        {"{}"}, {"{\"k\":\"u\"}"}, {"{\"k\":\"u\",\"b\":-1}"},
        {"{\"k\":\"u\",\"b\":1.5}"}, {"{\"k\":\"u\",\"b\":\"1\"}"},
        {"{\"k\":\"download\",\"b\":1}"}, {"{\"k\":\"u\",\"b\":1000000000000}"},
        {"{\"k\":\"u\",\"b\":1} {}"}, {"{\"k\":\"u\",\"b\":1}", true},
        {"{\"k\":\"u\",\"b\":1,\"padding\":\"" + QByteArray(64 << 10, 'x') + "\"}"},
    };
    for (const auto& test : cases) {
        WebSocketServer server([&](QWebSocket* socket) {
            auto sent = std::make_shared<bool>(false);
            QObject::connect(socket, &QWebSocket::binaryMessageReceived, socket, [socket, sent, test](const QByteArray&) {
                if (*sent) return;
                *sent = true;
                if (test.binary) socket->sendBinaryMessage(test.payload);
                else socket->sendTextMessage(QString::fromUtf8(test.payload));
            });
        });
        WorkerState state;
        puls::YandexUploadProbe probe;
        probe.websocketUrl = server.url();
        const auto opts = options(QUrl("http://127.0.0.1"));
        const auto previousFailures = failures;
        expectError([&] { puls::yandex_transfer::websocketUpload(probe, opts, 1000ms, state.context()); },
                    "protocol", "WebSocket rejects invalid ACK schema/type/size/outstanding count");
        if (failures != previousFailures) std::cerr << "ACK case size=" << test.payload.size() << ", binary=" << test.binary << '\n';
        expect(!state.ready && state.bytes == 0, "invalid WebSocket ACK is never counted");
    }
}

void testWebSocketDisconnectAndHandshakeTimeout() {
    WebSocketServer server([](QWebSocket* socket) {
        QObject::connect(socket, &QWebSocket::binaryMessageReceived, socket, [socket](const QByteArray&) { socket->abort(); });
    });
    WorkerState state;
    puls::YandexUploadProbe probe;
    probe.websocketUrl = server.url();
    const auto opts = options(QUrl("http://127.0.0.1"));
    expectError([&] { puls::yandex_transfer::websocketUpload(probe, opts, 1000ms, state.context()); },
                "unavailable", "WebSocket disconnect before deadline is an error");
    expect(state.bytes == 0 && !state.ready, "WebSocket disconnect cannot count sent bytes");

    HttpServer stalled([](QTcpSocket*, const Request&) {});
    probe.websocketUrl = stalled.url();
    probe.websocketUrl.setScheme("ws");
    probe.websocketConnectionTimeoutMs = 30;
    QElapsedTimer elapsed;
    elapsed.start();
    expectError([&] { puls::yandex_transfer::websocketUpload(probe, options(stalled.url()), 1000ms, state.context()); },
                "timeout", "WebSocket honors discovery connection timeout");
    expect(elapsed.elapsed() < 500, "WebSocket handshake timeout closes I/O promptly");
}

void testProtocolCancellation() {
    HttpServer stalled([](QTcpSocket*, const Request&) {});
    const auto opts = options(stalled.url());
    for (const bool upload : {false, true}) {
        WorkerState state;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &timer, [&] { state.stop = true; });
        timer.start(40);
        QElapsedTimer elapsed;
        elapsed.start();
        client([&] {
            QNetworkAccessManager manager;
            if (upload) puls::yandex_transfer::httpUpload(manager, stalled.url(), opts, state.context());
            else puls::yandex_transfer::downloadProbe(manager, stalled.url(), opts, state.context());
        });
        expect(elapsed.elapsed() < 500 && state.bytes == 0, "HTTP cancellation aborts stalled I/O without accounting");
    }
    WebSocketServer ws([](QWebSocket*) {});
    WorkerState state;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &timer, [&] { state.stop = true; });
    timer.start(40);
    puls::YandexUploadProbe probe;
    probe.websocketUrl = ws.url();
    QElapsedTimer elapsed;
    elapsed.start();
    client([&] { puls::yandex_transfer::websocketUpload(probe, opts, 1000ms, state.context()); });
    expect(elapsed.elapsed() < 500 && state.bytes == 0 && !state.ready,
           "WebSocket cancellation aborts I/O and never counts raw send bytes");
}

void testIntegratedDownloadAndReconnect() {
    int requests = 0;
    HttpServer server([&](QTcpSocket* socket, const Request&) {
        if (++requests == 1) { respond(socket, 503, "Content-Length: 0\r\n"); return; }
        QTimer::singleShot(2, socket, [socket] {
            respond(socket, 200, "Content-Type: application/octet-stream\r\nContent-Length: 4096\r\n", QByteArray(4096, 'x'));
        });
    });
    const auto opts = options(server.url());
    puls::Config config;
    config.connections = 1;
    puls::Cancellation cancellation;
    QJsonObject result;
    client([&] { result = puls::yandexDownload(config, cancellation, opts, {server.url()}); });
    expect(result.value("status") == "ok" && result.value("bytes").toInteger() > 0 &&
           result.value("bytes").toInteger() % 4096 == 0, "integrated download returns confirmed bytes");
    expect(requests > 2 && result.value("warnings").toArray().size() > 0,
           "integrated download reconnects once and retains worker warning");
}

void testIntegratedUploadFallback() {
    int httpRequests = 0;
    HttpServer http([&](QTcpSocket* socket, const Request& request) {
        expect(request.bodySize == (2 << 20), "fallback sends exact 2 MiB block");
        ++httpRequests;
        QTimer::singleShot(2, socket, [socket] { respond(socket, 204, "Content-Length: 0\r\n"); });
    });
    WebSocketServer ws([](QWebSocket* socket) {
        auto sent = std::make_shared<bool>(false);
        QObject::connect(socket, &QWebSocket::binaryMessageReceived, socket, [socket, sent](const QByteArray&) {
            if (!*sent) { *sent = true; socket->sendTextMessage(QStringLiteral("{\"k\":\"u\",\"b\":1000000000000}")); }
        });
    });
    puls::YandexUploadProbe probe;
    probe.postUrl = http.url();
    probe.websocketUrl = ws.url();
    const auto opts = options(http.url());
    puls::Config config;
    config.connections = 1;
    puls::Cancellation cancellation;
    QJsonObject result;
    client([&] { result = puls::yandexUpload(config, cancellation, opts, {probe}); });
    const auto bytes = result.value("bytes").toInteger();
    expect(result.value("status") == "ok" && bytes > 0 && bytes % (2 << 20) == 0,
           "integrated fallback counts only confirmed HTTP blocks");
    expect(httpRequests > 0 && ws.connections == 1, "integrated upload prefers WebSocket then advertised HTTP fallback");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        testDownloadConfirmedBytes();
        testDownloadRejectsInvalidResponses();
        testHTTPUploadConfirmedBlocks();
        testHTTPUploadRejectsInvalidResponses();
        testWebSocketCountsOnlyACK();
        testWebSocketRejectsInvalidACKs();
        testWebSocketDisconnectAndHandshakeTimeout();
        testProtocolCancellation();
        testIntegratedDownloadAndReconnect();
        testIntegratedUploadFallback();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected exception: " << error.what() << '\n';
        ++failures;
    }
    return failures == 0 ? 0 : 1;
}
