#include "puls/core.hpp"
#include "puls/network.hpp"

#include <QCoreApplication>
#include <QHostAddress>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QThread>
#include <QWebSocket>
#include <QWebSocketServer>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

namespace {

int failures = 0;
void expect(bool value, const char* label) {
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

class FallbackMock final : public QThread {
public:
    FallbackMock(int slowDelayMs, bool stall = false) : slowDelayMs_(slowDelayMs), stall_(stall) {
        start();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!ready_.load() && std::chrono::steady_clock::now() < until) QThread::msleep(2);
    }
    ~FallbackMock() override { stop(); }
    quint16 httpPort() const { return httpPort_.load(); }
    quint16 slowPort() const { return slowPort_.load(); }
    quint16 fastPort() const { return fastPort_.load(); }
    int nearestCalls() const { return nearestCalls_.load(); }
    int tokenCalls() const { return tokenCalls_.load(); }
    int throughputCalls() const { return throughputCalls_.load(); }
    int acceptedSockets() const { return acceptedSockets_.load(); }
    int pings() const { return pings_.load(); }
    void stop() { quit(); wait(); }

protected:
    void run() override {
        QTcpServer http;
        QWebSocketServer slow(QStringLiteral("puls-fallback-slow"), QWebSocketServer::NonSecureMode);
        QWebSocketServer fast(QStringLiteral("puls-fallback-fast"), QWebSocketServer::NonSecureMode);
        if (!http.listen(QHostAddress::LocalHost, 0) || !slow.listen(QHostAddress::LocalHost, 0) ||
            !fast.listen(QHostAddress::LocalHost, 0)) return;
        httpPort_.store(http.serverPort());
        slowPort_.store(slow.serverPort());
        fastPort_.store(fast.serverPort());
        QObject::connect(&http, &QTcpServer::newConnection, &http, [&] {
            while (http.hasPendingConnections()) {
                auto* socket = http.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const QByteArray request = socket->readAll();
                    const QByteArray line = request.left(request.indexOf("\r\n"));
                    const QByteArray path = line.split(' ').value(1).split('?').value(0);
                    int status = 503;
                    QByteArray reason = "Service Unavailable";
                    QByteArray body = "{}";
                    if (path == "/api/nearest_servers") ++nearestCalls_;
                    else if (path == "/api/server/gentoken") { ++tokenCalls_; status = 418; reason = "Unexpected"; }
                    else if (path == "/download.php" || path == "/upload.php") {
                        ++throughputCalls_; status = 418; reason = "Unexpected";
                    }
                    const QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " + reason +
                        "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
            }
        });
        auto wireQms = [this](QWebSocketServer& server, int delayMs) {
            QObject::connect(&server, &QWebSocketServer::newConnection, &server, [this, &server, delayMs] {
                while (server.hasPendingConnections()) {
                    QWebSocket* socket = server.nextPendingConnection();
                    socket->setParent(&server);
                    ++acceptedSockets_;
                    QObject::connect(socket, &QWebSocket::textMessageReceived, socket,
                        [this, socket, delayMs](const QString& message) {
                            if (stall_) return;
                            if (message == QStringLiteral("HI")) socket->sendTextMessage(QStringLiteral("HELLO"));
                            else if (message == QStringLiteral("GETINFO")) socket->sendTextMessage(QStringLiteral("QMS_TESTING"));
                            else if (message == QStringLiteral("AUTH")) socket->sendTextMessage(QStringLiteral("READY_TO_TEST"));
                            else if (message == QStringLiteral("PING")) {
                                ++pings_;
                                QTimer::singleShot(delayMs, socket, [socket] {
                                    if (socket->state() == QAbstractSocket::ConnectedState)
                                        socket->sendTextMessage(QStringLiteral("PONG 1"));
                                });
                            }
                        });
                }
            });
        };
        wireQms(slow, slowDelayMs_);
        wireQms(fast, 0);
        ready_.store(true);
        exec();
    }

private:
    int slowDelayMs_;
    bool stall_;
    std::atomic_bool ready_{false};
    std::atomic<quint16> httpPort_{0}, slowPort_{0}, fastPort_{0};
    std::atomic_int nearestCalls_{0}, tokenCalls_{0}, throughputCalls_{0}, acceptedSockets_{0}, pings_{0};
};

puls::BackendOptions optionsFor(const FallbackMock& server, const QStringList& hosts) {
    puls::BackendOptions options;
    options.speedtestBase = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.httpPort()));
    options.allowLoopbackHTTP = true;
    options.speedtestFallbackHosts = hosts;
    return options;
}

puls::Config config() {
    puls::Config result;
    result.service = QStringLiteral("speedtest");
    result.durationSeconds = 3;
    result.connections = 1;
    return result;
}

QString host(quint16 port) { return QStringLiteral("127.0.0.1:%1").arg(port); }

void testFallbackChoosesFastestAndIsPingOnly() {
    FallbackMock server(8);
    const auto options = optionsFor(server, {host(server.slowPort()), host(server.fastPort())});
    auto backend = puls::makeBackendForTesting(QStringLiteral("speedtest"), options);
    puls::Cancellation cancellation;
    const auto selected = backend->select(config(), cancellation);
    expect(selected.value("name").toString() == host(server.fastPort()), "fallback selects the server with the lower median ping");
    expect(server.nearestCalls() == 1, "fallback follows a failed nearest_servers request");
    expect(server.pings() == 20, "fallback performs ten QMS pings against each responsive candidate");
    const int acceptedBeforePing = server.acceptedSockets();
    const auto ping = backend->ping(config(), cancellation);
    expect(ping.value("status").toString() == "ok", "fallback ping succeeds from cached samples");
    expect(server.acceptedSockets() == acceptedBeforePing, "selected fallback reuses its ten cached samples");
    for (const auto& phase : {QStringLiteral("download"), QStringLiteral("upload")}) {
        bool rejected = false;
        try {
            if (phase == QStringLiteral("download")) (void)backend->download(config(), cancellation);
            else (void)backend->upload(config(), cancellation);
        } catch (const puls::OpError& error) {
            rejected = error.code == QStringLiteral("auth") && error.retryable &&
                QString::fromUtf8(error.what()).contains(QStringLiteral("только проверку задержки"));
        }
        expect(rejected, "fallback throughput phase returns a retryable ping-only error");
    }
    expect(server.tokenCalls() == 0 && server.throughputCalls() == 0,
           "fallback throughput errors happen without JWT or transfer requests");
}

void testFallbackCancellationStopsPromptly() {
    FallbackMock server(0, true);
    auto backend = puls::makeBackendForTesting(QStringLiteral("speedtest"),
        optionsFor(server, {host(server.slowPort())}));
    puls::Cancellation cancellation;
    std::thread cancelWhenConnected([&] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (server.acceptedSockets() == 0 && std::chrono::steady_clock::now() < until) QThread::msleep(2);
        cancellation.cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    bool canceled = false;
    try { (void)backend->select(config(), cancellation); }
    catch (const puls::OpError& error) { canceled = error.code == QStringLiteral("canceled"); }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    cancelWhenConnected.join();
    expect(canceled, "fallback selection preserves cancellation");
    expect(elapsed < std::chrono::seconds(1), "fallback cancellation aborts QMS probes within one second");
}

void testTestFactoryRejectsExternalFallbackHost() {
    FallbackMock server(0);
    bool rejected = false;
    try {
        auto options = optionsFor(server, {QStringLiteral("example.com:20000")});
        (void)puls::makeBackendForTesting(QStringLiteral("speedtest"), options);
    } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "test fallback host injection rejects non-loopback candidates");
}

void testTestFactoryDisablesProductionFallbackByDefault() {
    FallbackMock server(0);
    auto options = optionsFor(server, {});
    options.speedtestFallbackHosts.reset();
    auto backend = puls::makeBackendForTesting(QStringLiteral("speedtest"), options);
    bool failed = false;
    try { (void)backend->select(config(), puls::Cancellation{}); }
    catch (const puls::OpError&) { failed = true; }
    expect(failed, "test backend preserves nearest_servers failure when fallback targets are unspecified");
    expect(server.acceptedSockets() == 0 && server.tokenCalls() == 0,
           "test factory never contacts production QMS fallback hosts by default");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testFallbackChoosesFastestAndIsPingOnly();
    testFallbackCancellationStopsPromptly();
    testTestFactoryRejectsExternalFallbackHost();
    testTestFactoryDisablesProductionFallbackByDefault();
    if (failures) std::cerr << failures << " fallback test(s) failed\n";
    return failures ? 1 : 0;
}
