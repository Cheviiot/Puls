#include "puls/transfer_network.hpp"

#include <QCoreApplication>
#include <QHostAddress>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

namespace {

int failures = 0;
void expect(bool value, const char* label) {
    if (!value) { ++failures; std::cerr << "FAIL: " << label << '\n'; }
}

class MockServer final : public QThread {
public:
    enum class Mode { Normal, RefreshKey, BadJWT, BadDownloadType, BadDownloadEncoding,
                      BadUploadEncoding, DownloadServerError, RejectFirstDownload, StalledDownload,
                      ControlledDownload };
    explicit MockServer(Mode mode = Mode::Normal, std::atomic_bool* controlledRelease = nullptr)
        : mode_(mode), controlledRelease_(controlledRelease) {
        start();
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (port() == 0 && std::chrono::steady_clock::now() < until) QThread::msleep(2);
    }
    ~MockServer() override { stop(); }
    quint16 port() const { return port_.load(); }
    int downloads() const { return downloads_.load(); }
    int gentokenCalls() const { return gentokenCalls_.load(); }
    int pageCalls() const { return pageCalls_.load(); }
    int scriptCalls() const { return scriptCalls_.load(); }
    int uploadBytes() const { return uploadBytes_.load(); }

    void stop() {
        quit();
        wait();
    }

protected:
    void run() override {
        QTcpServer server;
        QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
            while (server.hasPendingConnections()) {
                QTcpSocket* socket = server.nextPendingConnection();
                auto buffer = std::make_shared<QByteArray>();
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
                    buffer->append(socket->readAll());
                    const auto split = buffer->indexOf("\r\n\r\n");
                    if (split < 0) return;
                    const QByteArray header = buffer->left(split);
                    const QByteArray requestLine = header.left(header.indexOf("\r\n"));
                    const QByteArray method = requestLine.split(' ').value(0);
                    const QByteArray path = requestLine.split(' ').value(1);
                    const int contentLength = headerValue(header, "content-length").toInt();
                    const int bodyStart = split + 4;
                    if (buffer->size() - bodyStart < contentLength) return;
                    const QByteArray body = buffer->mid(bodyStart, contentLength);
                    respond(socket, method, path, header, body);
                });
            }
        });
        if (!server.listen(QHostAddress::LocalHost, 0)) return;
        port_.store(server.serverPort());
        exec();
    }

private:
    static QByteArray headerValue(const QByteArray& headers, const QByteArray& name) {
        const auto rows = headers.split('\n');
        for (auto row : rows) {
            row = row.trimmed();
            const auto colon = row.indexOf(':');
            if (colon > 0 && row.left(colon).trimmed().toLower() == name.toLower()) return row.mid(colon + 1).trimmed();
        }
        return {};
    }

    void reply(QTcpSocket* socket, int status, const QByteArray& contentType, const QByteArray& body,
               const QByteArray& extraHeaders = {}) {
        const QByteArray reason = status == 200 ? "OK" : status == 204 ? "No Content" :
                                  status == 403 ? "Forbidden" : status == 401 ? "Unauthorized" : "Bad Request";
        QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\n";
        response += "Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (!contentType.isEmpty()) response += "Content-Type: " + contentType + "\r\n";
        response += extraHeaders + "\r\n" + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    void respond(QTcpSocket* socket, const QByteArray& method, const QByteArray& target,
                 const QByteArray& headers, const QByteArray& body) {
        const QByteArray path = target.split('?').value(0);
        if (path == "/api/server/gentoken") {
            ++gentokenCalls_;
            if (method != "POST") {
                reply(socket, 405, "application/json", "{}");
            } else if (mode_ == Mode::RefreshKey && headerValue(headers, "x-api-key") != "fresh-browser-key-123456") {
                reply(socket, 403, "application/json", "{}");
            } else if (mode_ == Mode::BadJWT) {
                reply(socket, 200, "application/json", R"({"token":"bad"})");
            } else {
                reply(socket, 200, "application/json", R"({"token":"good.payload.signature"})");
            }
            return;
        }
        if (path == "/" && method == "GET") {
            ++pageCalls_;
            reply(socket, 200, "text/html", "<script src=\"/app/test.js\"></script>");
            return;
        }
        if (path == "/app/test.js") {
            ++scriptCalls_;
            reply(socket, 200, "application/javascript", "QMSLibrary({id:\"fresh-browser-key-123456\"})");
            return;
        }
        if (path == "/download.php") {
            ++downloads_;
            if (headerValue(headers, "jwt") != "good.payload.signature") {
                reply(socket, 401, "text/plain", "unauthorized");
                return;
            }
            if (mode_ == Mode::RejectFirstDownload && downloads_.load() == 1) {
                reply(socket, 401, "text/plain", "expired");
                return;
            }
            if (mode_ == Mode::DownloadServerError) { reply(socket, 503, "text/plain", "unavailable"); return; }
            if (mode_ == Mode::StalledDownload) {
                socket->write("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 25000000\r\nContent-Type: application/octet-stream\r\n\r\n");
                socket->flush();
                return;
            }
            if (mode_ == Mode::ControlledDownload) {
                socket->write("HTTP/1.1 200 OK\r\nConnection: keep-alive\r\nContent-Length: 25000000\r\nContent-Type: application/octet-stream\r\n\r\n");
                socket->flush();
                auto* poll = new QTimer(socket);
                poll->setInterval(1);
                QObject::connect(poll, &QTimer::timeout, socket, [socket, poll, release = controlledRelease_] {
                    if (release && release->load()) {
                        socket->write(QByteArray(1024, 'x'));
                        socket->flush();
                        poll->stop();
                        poll->deleteLater();
                    }
                });
                poll->start();
                return;
            }
            bool ok = false;
            const auto query = target.split('?').value(1);
            int chunk = 0;
            for (const auto& item : query.split('&')) if (item.startsWith("ckSize=")) chunk = item.mid(7).toInt(&ok);
            if (!ok || chunk < 1 || chunk > 250) { reply(socket, 400, "text/plain", "bad chunk"); return; }
            const QByteArray data(chunk * 1000000, 'x');
            const QByteArray type = mode_ == Mode::BadDownloadType ? "text/html" : "application/octet-stream";
            const QByteArray encoding = mode_ == Mode::BadDownloadEncoding ? "Content-Encoding: gzip\r\n" : "";
            reply(socket, 200, type, data, encoding);
            return;
        }
        if (path == "/upload.php") {
            uploadBytes_.store(body.size());
            if (headerValue(headers, "jwt") != "good.payload.signature" || body.size() != 2 * 1024 * 1024) {
                reply(socket, 400, "text/plain", "bad upload");
            } else if (mode_ == Mode::BadUploadEncoding) {
                reply(socket, 204, {}, {}, "Content-Encoding: gzip\r\n");
            } else {
                reply(socket, 204, {}, {});
            }
            return;
        }
        reply(socket, 400, "text/plain", "unexpected endpoint");
    }

    Mode mode_;
    std::atomic_bool* controlledRelease_ = nullptr;
    std::atomic<quint16> port_{0};
    std::atomic_int downloads_{0};
    std::atomic_int gentokenCalls_{0};
    std::atomic_int pageCalls_{0};
    std::atomic_int scriptCalls_{0};
    std::atomic_int uploadBytes_{0};
};

puls::BackendOptions optionsFor(const MockServer& server) {
    puls::BackendOptions options;
    options.speedtestBase = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.port()));
    options.allowLoopbackHTTP = true;
    options.transferDuration = std::chrono::milliseconds(500);
    options.transferWarmup = std::chrono::milliseconds(0);
    options.transferReadyGrace = std::chrono::milliseconds(0);
    options.transferStartupTimeout = std::chrono::milliseconds(750);
    return options;
}

puls::Config config() {
    puls::Config value;
    value.service = QStringLiteral("speedtest");
    value.durationSeconds = 3;
    value.connections = 1;
    value.only = QStringLiteral("all");
    return value;
}

QList<QUrl> serversFor(const MockServer& server) {
    return {QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.port()))};
}

void testDownloadExactBytesAndRefresh() {
    MockServer server(MockServer::Mode::RefreshKey);
    puls::SpeedtestSession session;
    session.browserKey = QStringLiteral("obsolete-browser-key");
    const auto result = puls::speedtestDownload(config(), puls::Cancellation{}, optionsFor(server), serversFor(server), session);
    expect(result.value("status").toString() == "ok", "download succeeds with exact response size");
    expect(result.value("bytes").toVariant().toLongLong() > 0, "download counts confirmed bytes");
    expect(server.pageCalls() == 1 && server.scriptCalls() == 1, "browser key refresh is single and same-origin");
    expect(server.gentokenCalls() == 2, "JWT is refreshed after forbidden browser key exactly once");
}

void testDownloadCountsOnlyBytesReceivedDuringMeasurement() {
    std::atomic_bool release{false};
    std::atomic_int progressTicks{0};
    MockServer server(MockServer::Mode::ControlledDownload, &release);
    puls::SpeedtestSession session;
    const auto result = puls::speedtestDownload(config(), puls::Cancellation{}, optionsFor(server),
        serversFor(server), session, [&release, &progressTicks](const QJsonObject&) {
            if (progressTicks.fetch_add(1) == 0) release.store(true);
        });
    expect(result.value("status").toString() == "ok", "controlled download completes at the measurement deadline");
    expect(progressTicks.load() > 0 && release.load(), "controlled response is released by the first progress tick");
    expect(result.value("bytes").toVariant().toLongLong() == 1024,
           "download counts exactly the bytes streamed after the first progress tick");
}

void testDownloadRejectsUnsafeType() {
    MockServer server(MockServer::Mode::BadDownloadType);
    puls::SpeedtestSession session;
    bool rejected = false;
    try { (void)puls::speedtestDownload(config(), puls::Cancellation{}, optionsFor(server), serversFor(server), session); }
    catch (const puls::OpError&) { rejected = true; }
    expect(rejected, "download rejects unexpected content type");
}

void testDownloadRejectsEncodingAndServerError() {
    for (const auto mode : {MockServer::Mode::BadDownloadEncoding, MockServer::Mode::DownloadServerError}) {
        MockServer server(mode);
        puls::SpeedtestSession session;
        bool rejected = false;
        try { (void)puls::speedtestDownload(config(), puls::Cancellation{}, optionsFor(server), serversFor(server), session); }
        catch (const puls::OpError&) { rejected = true; }
        expect(rejected, "download rejects encoding and non-2xx status");
    }
}

void testDownloadRefreshesAfterUnauthorized() {
    MockServer server(MockServer::Mode::RejectFirstDownload);
    puls::SpeedtestSession session;
    const auto result = puls::speedtestDownload(config(), puls::Cancellation{}, optionsFor(server), serversFor(server), session);
    expect(result.value("status").toString() == "ok", "download retries once after 401 with refreshed JWT");
    expect(server.gentokenCalls() == 2, "401 refresh does not loop gentoken requests");
}

void testMalformedJWTIsRejected() {
    MockServer server(MockServer::Mode::BadJWT);
    puls::SpeedtestSession session;
    bool rejected = false;
    try { (void)puls::speedtestUpload(config(), puls::Cancellation{}, optionsFor(server), serversFor(server), session); }
    catch (const puls::OpError&) { rejected = true; }
    expect(rejected, "malformed gentoken JWT is rejected");
}

void testUploadCountsOnlySuccessfulBlock() {
    MockServer server;
    puls::SpeedtestSession session;
    const auto result = puls::speedtestUpload(config(), puls::Cancellation{}, optionsFor(server), serversFor(server), session);
    expect(result.value("status").toString() == "ok", "upload succeeds after server response");
    expect(server.uploadBytes() == 2 * 1024 * 1024, "upload block is exactly two MiB");
    expect(result.value("bytes").toVariant().toLongLong() >= 2 * 1024 * 1024, "upload counts only acknowledged blocks");
}

void testUploadRejectsEncodedResponse() {
    MockServer server(MockServer::Mode::BadUploadEncoding);
    puls::SpeedtestSession session;
    bool rejected = false;
    try { (void)puls::speedtestUpload(config(), puls::Cancellation{}, optionsFor(server), serversFor(server), session); }
    catch (const puls::OpError&) { rejected = true; }
    expect(rejected, "upload rejects compressed server response");
}

void testCancellationAbortsDownloadPromptly() {
    MockServer server(MockServer::Mode::StalledDownload);
    puls::SpeedtestSession session;
    puls::Cancellation cancellation;
    std::thread cancelWhenRequested([&] {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (server.downloads() == 0 && std::chrono::steady_clock::now() < until) QThread::msleep(2);
        cancellation.cancel();
    });
    const auto started = std::chrono::steady_clock::now();
    bool canceled = false;
    try { (void)puls::speedtestDownload(config(), cancellation, optionsFor(server), serversFor(server), session); }
    catch (const puls::OpError& error) { canceled = error.code == QStringLiteral("canceled"); }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    cancelWhenRequested.join();
    expect(elapsed < std::chrono::seconds(1), "download cancellation aborts network I/O within one second");
    expect(canceled, "canceled download does not report success");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    testDownloadExactBytesAndRefresh();
    testDownloadCountsOnlyBytesReceivedDuringMeasurement();
    testDownloadRejectsUnsafeType();
    testDownloadRejectsEncodingAndServerError();
    testDownloadRefreshesAfterUnauthorized();
    testMalformedJWTIsRejected();
    testUploadCountsOnlySuccessfulBlock();
    testUploadRejectsEncodedResponse();
    testCancellationAbortsDownloadPromptly();
    if (failures) std::cerr << failures << " speedtest transfer test(s) failed\n";
    return failures ? 1 : 0;
}
