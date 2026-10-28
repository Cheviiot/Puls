#include "puls/core.hpp"
#include "puls/network.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonValue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>
#include <QWebSocket>
#include <QWebSocketServer>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

bool near(double actual, double expected, double epsilon = 1e-9) {
    return std::abs(actual - expected) <= epsilon;
}

template <typename Fn>
void expectThrows(Fn&& fn, const char* label) {
    try {
        fn();
        expect(false, label);
    } catch (const std::exception&) {
    }
}

void testPublicErrorMessagesMatchSchemaOne() {
    QFile golden(QFileInfo(QString::fromUtf8(__FILE__)).dir().filePath("golden/errors.json"));
    expect(golden.open(QIODevice::ReadOnly), "error message golden file opens");
    const auto values = QJsonDocument::fromJson(golden.readAll()).array();
    expect(values.size() == 6, "all stable error codes have golden messages");
    for (const auto& item : values) {
        const auto expected = item.toObject();
        const QString code = expected.value("code").toString();
        const puls::OpError error(code, "download", "internal protocol cause", true);
        const auto value = error.json();
        expect(value.value("message") == expected.value("message"), "public error message matches Go schema-one contract");
        expect(value.value("code") == code && value.value("phase") == "download" && value.size() == 4,
               "public error JSON contains only stable fields");
        expect(error.message == "internal protocol cause", "typed cause retains internal diagnostics");
    }
}

void testParseMeasurementCommands() {
    struct Case {
        QStringList args;
        QString service;
        int duration = 0;
        int connections = 0;
        QString only;
    };
    const std::vector<Case> cases{
        {{}, "", 10, 0, "all"},
        {{"yandex"}, "yandex", 10, 0, "all"},
        {{"speedtest", "--profile", "quick", "--connections", "4"}, "speedtest", 5, 4, "all"},
        {{"all", "--duration", "12", "--only", "ping"}, "all", 12, 0, "ping"},
    };
    for (const auto& test : cases) {
        const auto config = puls::parseConfig(test.args);
        expect(config.command == puls::Command::Measure, "measurement command kind");
        expect(config.service == test.service, "measurement service parsing");
        expect(config.durationSeconds == test.duration, "measurement duration parsing");
        expect(config.connections == test.connections, "measurement connections parsing");
        expect(config.only == test.only, "measurement phase parsing");
    }

    const auto ip = puls::parseConfig({"ip"});
    expect(ip.command == puls::Command::IP && ip.service.isEmpty() && !ip.explicitService,
           "ip command defaults service without marking it explicit");
    const auto explicitIP = puls::parseConfig({"ip", "yandex", "--json"});
    expect(explicitIP.command == puls::Command::IP && explicitIP.service == "yandex" && explicitIP.explicitService,
           "ip command preserves explicit service");

    const auto help = puls::parseConfig({"-h"});
    expect(help.command == puls::Command::Help, "short help command");
    const auto version = puls::parseConfig({"--version"});
    expect(version.command == puls::Command::Version, "version flag command");
    const auto gui = puls::parseConfig({"gui", "--verbose"});
    expect(gui.command == puls::Command::GUI && gui.verbose, "gui command and verbose flag");
}

void testBooleanFlagSyntax() {
    auto config = puls::parseConfig({"yandex", "--json=true", "--verbose=false", "--show-ip=false", "--no-color=true"});
    expect(config.json, "boolean flag accepts explicit true");
    expect(!config.verbose, "boolean flag accepts explicit false");
    expect(!config.showIP, "show-ip false syntax");
    expect(config.noColor, "no-color true syntax");

    expectThrows([] { (void)puls::parseConfig({"yandex", "--json=maybe"}); },
                 "malformed boolean flag is rejected");
}

void testInvalidCLIArguments() {
    const std::vector<QStringList> cases{
        {"--ip"},
        {"ip", "all"},
        {"--duration", "2"},
        {"--duration", "61"},
        {"--connections", "17"},
        {"--profile", "slow"},
        {"--only", "latency"},
        {"yandex", "--server", "example.com"},
        {"all", "--server", "example.com"},
        {"unknown"},
    };
    for (const auto& args : cases) {
        expectThrows([&args] { (void)puls::parseConfig(args); }, "invalid or removed CLI option is rejected");
    }
}

void testPingStatsParity() {
    const QList<double> samples{
        10.0,
        std::numeric_limits<double>::quiet_NaN(),
        100.0,
        -2.0,
        20.0,
        std::numeric_limits<double>::infinity(),
        40.0,
    };
    const auto average = puls::pingPhase(samples, "average");
    expect(average.value("samples").toInt() == 4, "ping stats discard invalid samples");
    expect(near(average.value("min_ms").toDouble(), 10.0), "ping minimum ignores invalid samples");
    expect(near(average.value("median_ms").toDouble(), 30.0), "even ping median is midpoint");
    expect(near(average.value("average_ms").toDouble(), 42.5), "ping average matches Go stats");
    expect(near(average.value("jitter_ms").toDouble(), 63.33),
           "ping jitter averages successive valid sample deltas and rounds to schema precision");
    expect(near(average.value("value_ms").toDouble(), 42.5), "average is default primary ping value");

    const auto median = puls::pingPhase({10.0, 100.0, 20.0, 40.0}, "median");
    expect(near(median.value("value_ms").toDouble(), 30.0), "median method selects median primary value");
    const auto minimum = puls::pingPhase({10.0, 100.0, 20.0, 40.0}, "minimum");
    expect(near(minimum.value("value_ms").toDouble(), 10.0), "minimum method selects minimum primary value");
    const auto forcedMinimum = puls::pingPhase({10.0, 100.0, 20.0}, "average", true);
    expect(near(forcedMinimum.value("value_ms").toDouble(), 10.0), "minimumPrimary selects minimum value");

    const auto zero = puls::pingPhase({0.0, 1.0}, "average");
    expect(zero.value("samples").toInt() == 2 && zero.value("value_ms").toDouble() == 0.5,
           "zero RTT remains a valid sample");
    expectThrows([] {
        (void)puls::pingPhase({std::numeric_limits<double>::quiet_NaN(), -1.0}, "average");
    }, "ping with no valid samples fails instead of reporting zero");
}

struct BackendState {
    bool failSelect = false;
    bool failPing = false;
    bool failConnection = false;
    bool blockPingUntilCanceled = false;
    QJsonArray downloadWarnings;
    QJsonArray uploadWarnings;
    std::atomic_int selectCalls{0};
    std::atomic_int pingCalls{0};
    std::atomic_int downloadCalls{0};
    std::atomic_int uploadCalls{0};
    std::atomic_int connectionCalls{0};
    std::atomic_bool pingStarted{false};
};

class FakeBackend final : public puls::Backend {
public:
    FakeBackend(QString service, std::shared_ptr<BackendState> state)
        : service_(std::move(service)), state_(std::move(state)) {}

    QString id() const override { return service_; }

    QJsonObject select(const puls::Config&, const puls::Cancellation&) override {
        ++state_->selectCalls;
        if (state_->failSelect)
            throw puls::OpError("unavailable", "select", "fake select failure");
        return {{"name", "mock.example"}, {"city", QJsonValue::Null}, {"region", QJsonValue::Null}};
    }

    QJsonObject ping(const puls::Config&, const puls::Cancellation& cancellation) override {
        ++state_->pingCalls;
        state_->pingStarted.store(true);
        if (state_->blockPingUntilCanceled) {
            while (!cancellation.canceled()) QThread::msleep(2);
            puls::checkCanceled(cancellation, "ping");
        }
        if (state_->failPing)
            throw puls::OpError("unavailable", "ping", "fake ping failure");
        return puls::pingPhase({0.0, 2.0, 4.0}, "median");
    }

    QJsonObject download(const puls::Config&, const puls::Cancellation&) override {
        ++state_->downloadCalls;
        auto result = puls::emptyPhase("ok");
        result.insert("mbps", 100.0);
        result.insert("bytes", 12500000);
        result.insert("elapsed_ms", 1000.0);
        result.insert("successful_streams", 2);
        result.insert("failed_streams", 0);
        result.insert("warnings", state_->downloadWarnings);
        return result;
    }

    QJsonObject upload(const puls::Config&, const puls::Cancellation&) override {
        ++state_->uploadCalls;
        auto result = puls::emptyPhase("ok");
        result.insert("mbps", 50.0);
        result.insert("bytes", 6250000);
        result.insert("elapsed_ms", 1000.0);
        result.insert("successful_streams", 1);
        result.insert("failed_streams", 0);
        result.insert("warnings", state_->uploadWarnings);
        return result;
    }

    QJsonObject connection(const puls::Cancellation&) override {
        ++state_->connectionCalls;
        if (state_->failConnection)
            throw puls::OpError("unavailable", "connection", "fake connection failure");
        return {{"status", "ok"}, {"external_ip", "203.0.113.1"},
                {"isp", QJsonValue::Null}, {"detected_by", service_}, {"warnings", QJsonArray{}}};
    }

private:
    QString service_;
    std::shared_ptr<BackendState> state_;
};

using StateMap = std::map<QString, std::shared_ptr<BackendState>>;

std::shared_ptr<BackendState> stateFor(StateMap& states, const QString& service) {
    auto& state = states[service];
    if (!state) state = std::make_shared<BackendState>();
    return state;
}

puls::Runner runnerFor(StateMap& states) {
    return puls::Runner([&states](const QString& service) -> std::unique_ptr<puls::Backend> {
        return std::make_unique<FakeBackend>(service, stateFor(states, service));
    });
}

puls::Config measureConfig(QString service = "yandex", QString only = "ping") {
    puls::Config config;
    config.service = std::move(service);
    config.only = std::move(only);
    return config;
}

QJsonObject resultAt(const QJsonObject& envelope, int index = 0) {
    return envelope.value("results").toArray().at(index).toObject();
}

void testRunnerContinuesAllAfterServiceFailure() {
    StateMap states;
    stateFor(states, "yandex")->failSelect = true;
    auto config = measureConfig("all");
    const puls::Cancellation cancellation;
    const auto envelope = runnerFor(states).run(config, cancellation);
    const auto results = envelope.value("results").toArray();
    expect(envelope.value("status") == "partial", "all aggregate is partial after one service fails");
    expect(results.size() == 2, "all attempts both services after first failure");
    expect(results.at(0).toObject().value("status") == "error", "first service failure is retained");
    expect(results.at(1).toObject().value("status") == "ok", "second service still succeeds");
    expect(stateFor(states, "speedtest")->pingCalls == 1, "second service ping ran after first failure");
}

void testRunnerDoesNotTurnPingFailureIntoZeroSuccess() {
    StateMap states;
    stateFor(states, "yandex")->failPing = true;
    const puls::Cancellation cancellation;
    const auto envelope = runnerFor(states).run(measureConfig(), cancellation);
    const auto result = resultAt(envelope);
    const auto ping = result.value("phases").toObject().value("ping").toObject();
    expect(envelope.value("status") == "error" && result.value("status") == "error",
           "ping failure keeps measurement in error state");
    expect(ping.value("status") == "error" && ping.value("value_ms").isNull(),
           "ping failure does not fabricate a zero result");
    expect(ping.value("error").toObject().value("code") == "unavailable" &&
               ping.value("error").toObject().value("phase") == "ping",
           "ping error preserves structured code and phase");
    expect(puls::exitCode(envelope) == 1, "failed ping maps to error exit code");
}

void testRunnerPreservesNullsAndEmptyArraysInSchemaOne() {
    StateMap states;
    auto config = measureConfig();
    config.only = "ping";
    const puls::Cancellation cancellation;
    const auto envelope = runnerFor(states).run(config, cancellation);
    const auto result = resultAt(envelope);
    const auto phases = result.value("phases").toObject();
    const auto ping = phases.value("ping").toObject();
    const auto skippedUpload = phases.value("upload").toObject();
    const QStringList phaseFields{
        "status", "value_ms", "min_ms", "median_ms", "average_ms", "jitter_ms", "samples",
        "method", "mbps", "bytes", "elapsed_ms", "successful_streams", "failed_streams", "warnings", "error"};
    expect(envelope.value("schema_version").toInt() == 1, "Runner emits schema version one");
    expect(envelope.value("command") == "measure", "measurement envelope command field");
    expect(envelope.value("connection").isNull(), "connection is explicit null when not requested");
    expect(result.value("error").isNull(), "measurement error is explicit null on success");
    expect(result.value("warnings").isArray() && result.value("warnings").toArray().isEmpty(),
           "measurement warnings are an empty array");
    expect(result.value("server").toObject().value("city").isNull() &&
               result.value("server").toObject().value("region").isNull(),
           "unknown server location fields are explicit nulls");
    for (const auto& field : phaseFields) {
        expect(ping.contains(field), "successful phase includes every schema-one field");
        expect(skippedUpload.contains(field), "skipped phase includes every schema-one field");
    }
    expect(ping.value("error").isNull() && ping.value("mbps").isNull(),
           "unused ping phase fields are explicit nulls");
    expect(ping.value("warnings").isArray() && ping.value("warnings").toArray().isEmpty(),
           "phase warnings are an empty array");
    expect(skippedUpload.value("status") == "skipped" && skippedUpload.value("bytes").isNull(),
           "unrequested phase retains skipped status and null metrics");
}

void testConnectionFailureDoesNotChangeMeasurementAggregate() {
    StateMap states;
    stateFor(states, "yandex")->failConnection = true;
    auto config = measureConfig();
    config.showIP = true;
    const puls::Cancellation cancellation;
    const auto envelope = runnerFor(states).run(config, cancellation);
    expect(envelope.value("status") == "ok", "connection lookup failure does not alter measurement status");
    expect(envelope.value("connection").toObject().value("status") == "error",
           "connection lookup failure is still represented");
    expect(resultAt(envelope).value("status") == "ok", "measurement succeeds despite connection failure");
}

void testIPDefaultFallbackAndExplicitServiceScope() {
    {
        StateMap states;
        stateFor(states, "speedtest")->failConnection = true;
        puls::Config config;
        config.command = puls::Command::IP;
        const puls::Cancellation cancellation;
        const auto envelope = runnerFor(states).run(config, cancellation);
        const auto connection = envelope.value("connection").toObject();
        expect(envelope.value("status") == "ok", "default IP lookup succeeds through fallback");
        expect(connection.value("detected_by") == "yandex", "default IP lookup falls back speedtest to yandex");
        expect(stateFor(states, "speedtest")->connectionCalls == 1 &&
                   stateFor(states, "yandex")->connectionCalls == 1,
               "default IP lookup tries speedtest then yandex exactly once");
    }
    {
        StateMap states;
        stateFor(states, "speedtest")->failConnection = true;
        puls::Config config;
        config.command = puls::Command::IP;
        config.service = "speedtest";
        config.explicitService = true;
        const puls::Cancellation cancellation;
        const auto envelope = runnerFor(states).run(config, cancellation);
        expect(envelope.value("status") == "error", "explicit IP lookup failure remains an error");
        expect(stateFor(states, "speedtest")->connectionCalls == 1 &&
                   stateFor(states, "yandex")->connectionCalls == 0,
               "explicit IP service does not receive fallback");
    }
}

void testRunnerRunsOnlyRequestedPhase() {
    StateMap states;
    auto config = measureConfig("yandex", "download");
    const puls::Cancellation cancellation;
    const auto envelope = runnerFor(states).run(config, cancellation);
    const auto phases = resultAt(envelope).value("phases").toObject();
    expect(envelope.value("status") == "ok", "download-only measurement succeeds");
    expect(phases.value("ping").toObject().value("status") == "skipped", "ping is skipped when unrequested");
    expect(phases.value("download").toObject().value("status") == "ok", "requested download runs");
    expect(phases.value("upload").toObject().value("status") == "skipped", "upload is skipped when unrequested");
    expect(stateFor(states, "yandex")->pingCalls == 0 && stateFor(states, "yandex")->downloadCalls == 1 &&
               stateFor(states, "yandex")->uploadCalls == 0,
           "backend sees only the requested phase");
}

void testRunnerPreCanceledAndMidflightCancellation() {
    {
        StateMap states;
        puls::Cancellation cancellation;
        cancellation.cancel();
        const auto started = std::chrono::steady_clock::now();
        const auto envelope = runnerFor(states).run(measureConfig(), cancellation);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        expect(envelope.value("status") == "canceled" && puls::exitCode(envelope) == 130,
               "pre-canceled measurement returns canceled status and exit code");
        expect(elapsed < std::chrono::seconds(1), "pre-canceled measurement returns within one second");
    }
    {
        StateMap states;
        auto state = stateFor(states, "yandex");
        state->blockPingUntilCanceled = true;
        const auto runner = runnerFor(states);
        const auto config = measureConfig();
        puls::Cancellation cancellation;
        QJsonObject envelope;
        const auto started = std::chrono::steady_clock::now();
        std::thread worker([&] { envelope = runner.run(config, cancellation); });
        const auto deadline = started + std::chrono::seconds(1);
        while (!state->pingStarted.load() && std::chrono::steady_clock::now() < deadline)
            QThread::msleep(2);
        const bool enteredPing = state->pingStarted.load();
        const auto cancellationRequestedAt = std::chrono::steady_clock::now();
        cancellation.cancel();
        worker.join();
        const auto elapsed = std::chrono::steady_clock::now() - cancellationRequestedAt;
        expect(enteredPing, "cancellation fake reached the active ping phase");
        expect(envelope.value("status") == "canceled", "midflight cancellation is represented");
        expect(elapsed < std::chrono::seconds(1), "midflight cancellation completes within one second");
    }
}

void testRunnerMarksOnlyRequestedPhasesCanceled() {
    const auto runCanceledPing = [](QString only) {
        StateMap states;
        auto state = stateFor(states, "yandex");
        state->blockPingUntilCanceled = true;
        const auto runner = runnerFor(states);
        const auto config = measureConfig("yandex", std::move(only));
        puls::Cancellation cancellation;
        QJsonObject envelope;
        std::thread worker([&] { envelope = runner.run(config, cancellation); });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!state->pingStarted.load() && std::chrono::steady_clock::now() < deadline)
            QThread::msleep(2);
        const bool enteredPing = state->pingStarted.load();
        cancellation.cancel();
        worker.join();
        expect(enteredPing, "requested ping entered before cancellation");
        expect(envelope.value("status") == "canceled", "canceled requested ping cancels envelope");
        return resultAt(envelope).value("phases").toObject();
    };

    const auto allPhases = runCanceledPing("all");
    for (const auto* name : {"ping", "download", "upload"})
        expect(allPhases.value(name).toObject().value("status") == "canceled",
               "active and remaining requested phases report canceled");

    const auto pingOnly = runCanceledPing("ping");
    expect(pingOnly.value("ping").toObject().value("status") == "canceled",
           "requested ping reports cancellation");
    expect(pingOnly.value("download").toObject().value("status") == "skipped" &&
               pingOnly.value("upload").toObject().value("status") == "skipped",
           "unrequested throughput phases remain skipped after cancellation");
}

void testRunnerAggregatesUniquePhaseWarnings() {
    StateMap states;
    auto state = stateFor(states, "yandex");
    state->downloadWarnings = QJsonArray{"shared warning", "download warning"};
    state->uploadWarnings = QJsonArray{"shared warning", "upload warning"};
    const puls::Cancellation cancellation;
    const auto envelope = runnerFor(states).run(measureConfig("yandex", "all"), cancellation);
    const auto result = resultAt(envelope);
    const auto warnings = result.value("warnings").toArray();
    expect(warnings == QJsonArray{"shared warning", "download warning", "upload warning"},
           "measurement warnings merge phase warnings once in first-seen order");
    const auto phases = result.value("phases").toObject();
    expect(phases.value("download").toObject().value("warnings").toArray().size() == 2 &&
               phases.value("upload").toObject().value("warnings").toArray().size() == 2,
           "phase warnings remain present in their phase results");
}

class LocalHttpServer final : public QTcpServer {
public:
    struct Response {
        QByteArray status;
        QByteArray contentType;
        QByteArray body;
        int delayMs = 0;
        bool holdOpen = false;
    };
    using Handler = std::function<Response(const QByteArray&)>;

    explicit LocalHttpServer(Handler handler) : handler_(std::move(handler)) {
        if (!listen(QHostAddress::LocalHost, 0))
            throw std::runtime_error("could not start loopback HTTP test server");
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (hasPendingConnections()) {
                auto* socket = nextPendingConnection();
                auto request = std::make_shared<QByteArray>();
                auto handled = std::make_shared<bool>(false);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket, request, handled] {
                    if (*handled) { socket->readAll(); return; }
                    request->append(socket->readAll());
                    const auto headerEnd = request->indexOf("\r\n\r\n");
                    if (headerEnd < 0) return;
                    qint64 bodySize = 0;
                    for (auto row : request->left(headerEnd).split('\n')) {
                        row = row.trimmed();
                        if (row.toLower().startsWith("content-length:")) bodySize = row.mid(15).trimmed().toLongLong();
                    }
                    // Reply only after the request is complete. An early 503
                    // while the client is still writing a 2 MiB upload makes
                    // Qt's transport retry behavior platform dependent.
                    if (request->size() - headerEnd - 4 < bodySize) return;
                    *handled = true;
                    const auto firstLine = request->left(request->indexOf("\r\n"));
                    const auto response = handler_(firstLine);
                    if (response.holdOpen) return;
                    const auto& body = response.body;
                    auto bytes = QByteArray("HTTP/1.1 ") + response.status + "\r\nContent-Type: " +
                        response.contentType + "\r\nContent-Length: " + QByteArray::number(body.size()) +
                        "\r\nConnection: close\r\n\r\n" + body;
                    auto send = [socket, bytes = std::move(bytes)] {
                        if (socket->state() == QAbstractSocket::ConnectedState) {
                            socket->write(bytes);
                            socket->disconnectFromHost();
                        }
                    };
                    if (response.delayMs > 0) QTimer::singleShot(response.delayMs, socket, std::move(send));
                    else send();
                });
            }
        });
    }

    QUrl baseUrl() const {
        return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(serverPort()));
    }

private:
    Handler handler_;
};

class QmsMockServer final {
public:
    enum class Mode { Valid, MalformedText, Binary, EarlyDisconnect };

    QmsMockServer(Mode mode, QList<int> pingDelays = {})
        : server_(QStringLiteral("Puls QMS test"), QWebSocketServer::NonSecureMode),
          mode_(mode), pingDelays_(std::move(pingDelays)) {
        if (!server_.listen(QHostAddress::LocalHost, 0))
            throw std::runtime_error("could not start loopback QMS test server");
        QObject::connect(&server_, &QWebSocketServer::newConnection, &server_, [this] {
            while (server_.hasPendingConnections()) {
                auto* socket = server_.nextPendingConnection();
                socket->setParent(&server_);
                QObject::connect(socket, &QWebSocket::textMessageReceived, socket, [this, socket](const QString& message) {
                    if (message == "HI") socket->sendTextMessage("HELLO");
                    else if (message == "GETINFO") socket->sendTextMessage("qms_testing");
                    else if (message == "AUTH") socket->sendTextMessage("READY_TO_TEST");
                    else if (message == "PING") respondToPing(socket);
                });
            }
        });
    }

    quint16 port() const { return server_.serverPort(); }
    int pingCount() const { return pingCount_; }

private:
    void respondToPing(QWebSocket* socket) {
        const int ping = pingCount_++;
        if (mode_ == Mode::MalformedText) {
            socket->sendTextMessage("PONG -1");
            return;
        }
        if (mode_ == Mode::Binary) {
            socket->sendBinaryMessage("PONG 1");
            return;
        }
        if (mode_ == Mode::EarlyDisconnect) {
            socket->abort();
            return;
        }
        const int delay = ping < pingDelays_.size() ? pingDelays_[ping] : 0;
        QTimer::singleShot(delay, socket, [socket, ping] {
            if (socket->state() == QAbstractSocket::ConnectedState)
                socket->sendTextMessage(QStringLiteral("PONG %1").arg(ping + 1));
        });
    }

    QWebSocketServer server_;
    Mode mode_;
    QList<int> pingDelays_;
    int pingCount_ = 0;
};

struct OperationError {
    QString code;
    QString phase;
    bool retryable = false;
};

template <typename Fn>
std::optional<OperationError> captureOpError(Fn&& operation) {
    try {
        operation();
    } catch (const puls::OpError& error) {
        return OperationError{error.code, error.phase, error.retryable};
    }
    return std::nullopt;
}

puls::BackendOptions localBackendOptions(const QUrl& base) {
    puls::BackendOptions options;
    options.yandexBase = base;
    options.speedtestBase = base;
    options.allowLoopbackHTTP = true;
    return options;
}

QJsonObject yandexDiscovery(const QString& origin, bool externalLatency = false) {
    const auto latency = (externalLatency ? QStringLiteral("http://example.com/internet/ping")
                                          : origin + QStringLiteral("/internet/ping"));
    return {
        {"mid", "local-test"},
        {"latency", QJsonObject{{"probes", QJsonArray{QJsonObject{{"url", latency}}}}}},
        {"download", QJsonObject{{"probes", QJsonArray{QJsonObject{
             {"url", origin + QStringLiteral("/internet/probes/50mb")}, {"timeout", 0}}}}}},
        {"upload", QJsonObject{{"probes", QJsonArray{QJsonObject{
             {"url", origin + QStringLiteral("/internet/upload")},
             {"postUrl", origin + QStringLiteral("/internet/post")},
             {"statsUrl", origin + QStringLiteral("/internet/stats")},
             {"size", 1024}, {"timeout", 0}}}}}},
    };
}

QJsonObject runBackendWithEvents(const puls::Config& request, const puls::BackendOptions& options) {
    puls::Cancellation cancellation;
    std::atomic_bool finished{false};
    QJsonObject envelope;
    std::thread worker([&] {
        puls::Runner runner([&](const QString& id) { return puls::makeBackendForTesting(id, options); });
        envelope = runner.run(request, cancellation);
        finished.store(true);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!finished.load() && std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    if (!finished.load()) cancellation.cancel();
    worker.join();
    expect(envelope.value("status") != "canceled", "backend integration mock completes before its timeout");
    return envelope;
}

void testRunnerRoutesNativeThroughputAfterDiscovery() {
    QByteArray discovery;
    int downloadRequests = 0, uploadRequests = 0;
    LocalHttpServer server([&](const QByteArray& line) {
        if (line.startsWith("GET /internet/api/v0/get-probes?"))
            return LocalHttpServer::Response{"200 OK", "application/json", discovery};
        if (line.startsWith("GET /internet/ping?"))
            return LocalHttpServer::Response{"200 OK", "text/plain", "ok"};
        if (line.startsWith("GET /internet/probes/50mb?")) ++downloadRequests;
        if (line.startsWith("POST /internet/post?")) ++uploadRequests;
        return LocalHttpServer::Response{"503 Service Unavailable", "text/plain", "mock unavailable"};
    });
    discovery = QJsonDocument(yandexDiscovery(server.baseUrl().toString())).toJson(QJsonDocument::Compact);
    auto options = localBackendOptions(server.baseUrl());
    options.transferDuration = std::chrono::milliseconds(40);
    options.transferWarmup = std::chrono::milliseconds(0);
    options.transferReadyGrace = std::chrono::milliseconds(0);
    options.transferStartupTimeout = std::chrono::milliseconds(1000);
    auto request = measureConfig("yandex", "all");
    request.connections = 1;
    const auto envelope = runBackendWithEvents(request, options);
    const auto phases = resultAt(envelope).value("phases").toObject();
    expect(downloadRequests == 2 && uploadRequests == 2,
           "Yandex backend retains discovery URLs and routes both transfers with one reconnect");
    expect(phases.value("ping").toObject().value("status") == "ok" && envelope.value("status") == "partial",
           "throughput failure preserves the successful native ping");
    for (const auto* phase : {"download", "upload"}) {
        const auto value = phases.value(phase).toObject();
        expect(value.value("status") == "error" && value.value("mbps").isNull(),
               "failed native transfer has no invented zero speed");
    }
}

void testRunnerRoutesExplicitQmsThroughput() {
    int tokens = 0, transfers = 0;
    LocalHttpServer server([&](const QByteArray& line) {
        if (line.startsWith("POST /api/server/gentoken?")) {
            ++tokens;
            return LocalHttpServer::Response{"200 OK", "application/json", "{\"token\":\"mock-header.mock-payload.mock-signature\"}"};
        }
        if (line.startsWith("GET /download.php?") || line.startsWith("POST /upload.php?")) ++transfers;
        return LocalHttpServer::Response{"503 Service Unavailable", "text/plain", "mock unavailable"};
    });
    auto options = localBackendOptions(server.baseUrl());
    options.transferDuration = std::chrono::milliseconds(40);
    options.transferWarmup = std::chrono::milliseconds(0);
    options.transferReadyGrace = std::chrono::milliseconds(0);
    options.transferStartupTimeout = std::chrono::milliseconds(1000);
    for (const auto* phase : {"download", "upload"}) {
        auto request = measureConfig("speedtest", phase);
        request.server = server.baseUrl().authority();
        request.explicitService = true;
        request.connections = 1;
        const auto envelope = runBackendWithEvents(request, options);
        const auto value = resultAt(envelope).value("phases").toObject().value(phase).toObject();
        expect(value.value("status") == "error" && value.value("mbps").isNull(),
               "QMS transfer failure stays typed and has no numeric speed");
    }
    if (tokens != 2 || transfers != 4)
        std::cerr << "QMS routing mock: tokens=" << tokens << ", transfers=" << transfers << '\n';
    expect(tokens == 2 && transfers == 4,
           "explicit QMS backend uses JWT POST and routes both transfer methods to the selected host");
}

void testYandexBackendDiscoversAgainstLoopbackMock() {
    int requests = 0;
    QByteArray discovery;
    LocalHttpServer server([&](const QByteArray& requestLine) {
        ++requests;
        expect(requestLine.startsWith("GET /internet/api/v0/get-probes?"),
               "Yandex discovery requests the first-party discovery path");
        return LocalHttpServer::Response{"200 OK", "application/json", discovery};
    });
    const auto origin = server.baseUrl().toString();
    discovery = QJsonDocument(yandexDiscovery(origin)).toJson(QJsonDocument::Compact);
    puls::BackendOptions options;
    options.yandexBase = server.baseUrl();
    options.speedtestBase = server.baseUrl();
    options.allowLoopbackHTTP = true;
    auto backend = puls::makeBackendForTesting("yandex", options);
    const puls::Cancellation cancellation;
    const auto selected = backend->select(measureConfig(), cancellation);
    expect(selected.value("name").toString() == QStringLiteral("127.0.0.1:%1").arg(server.serverPort()),
           "Yandex selects validated loopback probe host");
    expect(requests == 1, "Yandex discovery made one local HTTP request");
}

void testYandexDiscoveryRejectsExternalInsecureProbeURL() {
    int requests = 0;
    QByteArray discovery;
    LocalHttpServer server([&](const QByteArray&) {
        ++requests;
        return LocalHttpServer::Response{"200 OK", "application/json", discovery};
    });
    discovery = QJsonDocument(yandexDiscovery(server.baseUrl().toString(), true)).toJson(QJsonDocument::Compact);
    puls::BackendOptions options;
    options.yandexBase = server.baseUrl();
    options.speedtestBase = server.baseUrl();
    options.allowLoopbackHTTP = true;
    auto backend = puls::makeBackendForTesting("yandex", options);
    const puls::Cancellation cancellation;
    bool rejected = false;
    try {
        (void)backend->select(measureConfig(), cancellation);
    } catch (const puls::OpError& error) {
        rejected = error.code == "protocol" && error.phase == "select";
    }
    expect(rejected, "Yandex discovery rejects a non-loopback insecure probe URL");
    expect(requests == 1, "rejected external probe was never requested");
}

void testYandexConnectionParsesBoundedBootstrapIP() {
    int requests = 0;
    LocalHttpServer server([&](const QByteArray& requestLine) {
        ++requests;
        expect(requestLine.startsWith("GET /internet/"), "Yandex connection reads the official Internetometer page");
        return LocalHttpServer::Response{"200 OK", "text/html",
            "<html>Client.default({\"ip\":{\"v4\":\"203.0.113.9\"}});</html>"};
    });
    puls::BackendOptions options;
    options.yandexBase = server.baseUrl();
    options.speedtestBase = server.baseUrl();
    options.allowLoopbackHTTP = true;
    auto backend = puls::makeBackendForTesting("yandex", options);
    const puls::Cancellation cancellation;
    const auto connection = backend->connection(cancellation);
    expect(connection.value("status") == "ok", "Yandex bootstrap IP response succeeds");
    expect(connection.value("external_ip") == "203.0.113.9", "Yandex extracts only the bootstrap IP");
    expect(connection.value("isp").isNull(), "Yandex connection response does not infer ISP");
    expect(requests == 1, "Yandex IP detection issues one local page request");
}

void testSpeedtestConnectionUsesIPThenOptionalASNEndpoints() {
    int ipRequests = 0;
    int asnRequests = 0;
    LocalHttpServer server([&](const QByteArray& requestLine) {
        if (requestLine.startsWith("GET /api/asn_provider/ip?")) {
            ++ipRequests;
            return LocalHttpServer::Response{"200 OK", "application/json", "{\"ip\":\"203.0.113.11\"}"};
        }
        if (requestLine.startsWith("GET /api/asn_provider/asn?")) {
            ++asnRequests;
            return LocalHttpServer::Response{"200 OK", "application/json", "{\"ip\":\"203.0.113.11\",\"provider_name\":\"Example ISP\"}"};
        }
        return LocalHttpServer::Response{"404 Not Found", "application/json", "{}"};
    });
    puls::BackendOptions options;
    options.speedtestBase = server.baseUrl();
    options.yandexBase = server.baseUrl();
    options.allowLoopbackHTTP = true;
    auto backend = puls::makeBackendForTesting("speedtest", options);
    const puls::Cancellation cancellation;
    const auto connection = backend->connection(cancellation);
    expect(connection.value("status") == "ok", "speedtest connection endpoint succeeds");
    expect(connection.value("external_ip") == "203.0.113.11", "speedtest IP endpoint is required and parsed");
    expect(connection.value("isp") == "Example ISP", "matching ASN response contributes ISP");
    expect(ipRequests == 1 && asnRequests == 1, "speedtest calls IP before optional ASN once");
}

void testSpeedtestRejectsISPOver255UTF8BytesButKeepsIP() {
    const QString oversizedISP(128, QChar(0x044F)); // 128 Cyrillic characters = 256 UTF-8 bytes.
    expect(oversizedISP.toUtf8().size() > 255 && oversizedISP.size() <= 255,
           "ISP fixture exceeds the byte limit without exceeding the character limit");
    const auto asnBody = QJsonDocument(QJsonObject{
        {"ip", "203.0.113.21"}, {"provider_name", oversizedISP}}).toJson(QJsonDocument::Compact);
    LocalHttpServer server([&](const QByteArray& requestLine) {
        if (requestLine.startsWith("GET /api/asn_provider/ip?"))
            return LocalHttpServer::Response{"200 OK", "application/json", "{\"ip\":\"203.0.113.21\"}"};
        if (requestLine.startsWith("GET /api/asn_provider/asn?"))
            return LocalHttpServer::Response{"200 OK", "application/json", asnBody};
        return LocalHttpServer::Response{"404 Not Found", "application/json", "{}"};
    });
    auto backend = puls::makeBackendForTesting("speedtest", localBackendOptions(server.baseUrl()));
    const puls::Cancellation cancellation;
    const auto connection = backend->connection(cancellation);
    expect(connection.value("status") == "ok" && connection.value("external_ip") == "203.0.113.21",
           "oversized ISP metadata does not invalidate successful IP detection");
    expect(connection.value("isp").isNull(), "ISP exceeding 255 UTF-8 bytes is omitted");
    expect(connection.value("warnings").toArray().size() == 1,
           "omitted oversized ISP metadata produces a connection warning");
}

void testSpeedtestSelectsFasterLaterQmsServer() {
    QList<int> slowDelays, fastDelays;
    for (int i = 0; i < 10; ++i) { slowDelays.append(50); fastDelays.append(1); }
    QmsMockServer slower(QmsMockServer::Mode::Valid, slowDelays);
    QmsMockServer faster(QmsMockServer::Mode::Valid, fastDelays);
    LocalHttpServer server([&](const QByteArray& requestLine) {
        if (!requestLine.startsWith("GET /api/nearest_servers?"))
            return LocalHttpServer::Response{"404 Not Found", "application/json", "{}"};
        const QJsonObject slow{
            {"id", 1}, {"src", "http://127.0.0.1"}, {"port", slower.port()},
            {"name", "slower"}, {"city", "Slow City"}, {"region_name", "Slow Region"}, {"source", "mock"}};
        const QJsonObject fast{
            {"id", 2}, {"src", "http://127.0.0.1"}, {"port", faster.port()},
            {"name", "faster"}, {"city", "Fast City"}, {"region_name", "Fast Region"}, {"source", "mock"}};
        const auto body = QJsonDocument(QJsonObject{
            {"algorithm", "mock"}, {"data", QJsonArray{slow, fast}}}).toJson(QJsonDocument::Compact);
        return LocalHttpServer::Response{"200 OK", "application/json", body};
    });
    auto backend = puls::makeBackendForTesting("speedtest", localBackendOptions(server.baseUrl()));
    const puls::Cancellation cancellation;
    const auto selected = backend->select(measureConfig("speedtest", "ping"), cancellation);
    expect(selected.value("name") == QStringLiteral("127.0.0.1:%1").arg(faster.port()),
           "speedtest ranks a later, faster QMS server ahead of the first slower server");
    expect(slower.pingCount() == 10 && faster.pingCount() == 10,
           "ranking samples each candidate with ten QMS pings");
}

void testDiscoveryRejectsWrongContentTypeAndOversizedBody() {
    const std::vector<std::pair<LocalHttpServer::Response, bool>> cases{
        {{"200 OK", "text/plain", "{}"}, true},
        {{"200 OK", "application/json", QByteArray(2 * 1024 * 1024 + 1, 'x')}, true},
    };
    for (const auto& [response, retryable] : cases) {
        LocalHttpServer server([&](const QByteArray&) { return response; });
        auto backend = puls::makeBackendForTesting("yandex", localBackendOptions(server.baseUrl()));
        const puls::Cancellation cancellation;
        const auto error = captureOpError([&] { (void)backend->select(measureConfig(), cancellation); });
        expect(error.has_value(), "invalid Yandex discovery response throws typed error");
        if (error) {
            expect(error->code == "protocol" && error->phase == "select",
                   "invalid Yandex discovery response is classified as protocol/select");
            expect(error->retryable == retryable, "Yandex discovery retry classification matches Go semantics");
        }
    }
}

void testDiscoveryAuthStatusClassificationAndKeyRefresh() {
    for (const auto status : {QByteArray("401 Unauthorized"), QByteArray("403 Forbidden")}) {
        int apiRequests = 0, pageRequests = 0, bundleRequests = 0;
        LocalHttpServer server([&](const QByteArray& requestLine) {
            if (requestLine.startsWith("GET /api/asn_provider/ip?")) {
                ++apiRequests;
                return LocalHttpServer::Response{status, "application/json", "{}"};
            }
            if (requestLine.startsWith("GET / ")) {
                ++pageRequests;
                return LocalHttpServer::Response{"200 OK", "text/html", "<script src=\"/bundle.js\"></script>"};
            }
            if (requestLine.startsWith("GET /bundle.js")) {
                ++bundleRequests;
                return LocalHttpServer::Response{"200 OK", "application/javascript", "QMSLibrary({id:'0123456789abcdef'});"};
            }
            return LocalHttpServer::Response{"404 Not Found", "application/json", "{}"};
        });
        auto backend = puls::makeBackendForTesting("speedtest", localBackendOptions(server.baseUrl()));
        const puls::Cancellation cancellation;
        const auto error = captureOpError([&] { (void)backend->connection(cancellation); });
        expect(error.has_value(), "persistent speedtest authorization error throws typed error");
        if (error) {
            expect(error->code == "auth" && error->phase == "connection",
                   "401 and 403 are classified as auth/connection");
            expect(error->retryable, "HTTP auth errors preserve Go retryable classification");
        }
        expect(apiRequests == 2 && pageRequests == 1 && bundleRequests == 1,
               "speedtest refreshes browser key once and retries the original request once");
    }
}

void testRealSocketCancellationClosesPendingDiscovery() {
    std::atomic_bool requestSeen{false};
    LocalHttpServer server([&](const QByteArray&) {
        requestSeen.store(true);
        return LocalHttpServer::Response{"200 OK", "application/json", {}, 0, true};
    });
    auto backend = puls::makeBackendForTesting("yandex", localBackendOptions(server.baseUrl()));
    puls::Cancellation cancellation;
    std::chrono::steady_clock::time_point cancellationRequestedAt;
    std::thread canceler([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!requestSeen.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        cancellationRequestedAt = std::chrono::steady_clock::now();
        cancellation.cancel();
    });
    const auto error = captureOpError([&] { (void)backend->select(measureConfig(), cancellation); });
    canceler.join();
    expect(requestSeen.load(), "cancellation test reached a real pending loopback HTTP request");
    expect(error && error->code == "canceled" && error->phase == "select",
           "pending loopback HTTP request reports typed cancellation");
    expect(std::chrono::steady_clock::now() - cancellationRequestedAt < std::chrono::seconds(1),
           "pending HTTP request closes within one second after cancellation");
}

void testYandexCDNsCollectLatencyIndependently() {
    std::atomic_int fastSamples{0};
    std::atomic_int slowSamples{0};
    quint16 listeningPort = 0;
    LocalHttpServer server([&](const QByteArray& requestLine) {
        if (requestLine.startsWith("GET /internet/api/v0/get-probes?")) {
            const auto port = listeningPort;
            const QString fast = QStringLiteral("http://127.0.0.1:%1/fast").arg(port);
            const QString slow = QStringLiteral("http://localhost:%1/slow").arg(port);
            QJsonObject discovery{
                {"mid", "local-test"},
                {"latency", QJsonObject{{"probes", QJsonArray{
                    QJsonObject{{"url", fast + "/ping"}}, QJsonObject{{"url", slow + "/ping"}}}}}},
                {"download", QJsonObject{{"probes", QJsonArray{
                    QJsonObject{{"url", fast + "/internet/probes/50mb"}, {"timeout", 0}},
                    QJsonObject{{"url", slow + "/internet/probes/50mb"}, {"timeout", 0}}}}}},
                {"upload", QJsonObject{{"probes", QJsonArray{
                    QJsonObject{{"url", fast + "/internet/upload"}, {"postUrl", fast + "/internet/post"}, {"statsUrl", fast + "/internet/stats"}, {"size", 1024}, {"timeout", 0}},
                    QJsonObject{{"url", slow + "/internet/upload"}, {"postUrl", slow + "/internet/post"}, {"statsUrl", slow + "/internet/stats"}, {"size", 1024}, {"timeout", 0}}}}}}
            };
            return LocalHttpServer::Response{"200 OK", "application/json", QJsonDocument(discovery).toJson(QJsonDocument::Compact)};
        }
        if (requestLine.startsWith("GET /fast/ping")) {
            const int index = fastSamples.fetch_add(1);
            return LocalHttpServer::Response{"200 OK", "text/plain", "ok", 20 + index * 20};
        }
        if (requestLine.startsWith("GET /slow/ping")) {
            slowSamples.fetch_add(1);
            return LocalHttpServer::Response{"200 OK", "text/plain", "ok", 250};
        }
        return LocalHttpServer::Response{"404 Not Found", "application/json", "{}"};
    });
    listeningPort = server.serverPort();
    auto backend = puls::makeBackendForTesting("yandex", localBackendOptions(server.baseUrl()));
    const puls::Cancellation cancellation;
    (void)backend->select(measureConfig(), cancellation);
    const auto phase = backend->ping(measureConfig(), cancellation);
    expect(fastSamples.load() == 4 && slowSamples.load() == 4,
           "Yandex samples both CDN latency probes four times");
    expect(phase.value("samples").toInt() == 8, "Yandex reports all valid samples across CDNs");
    expect(phase.value("value_ms").toDouble() < 150.0,
           "fast CDN RTT is measured independently of slower CDN response delay");
    expect(phase.value("jitter_ms").toDouble() < 100.0,
           "Yandex jitter uses only the winning CDN samples");
}

void testQmsLocalWebSocketHandshakeAndFailures() {
    {
        QmsMockServer server(QmsMockServer::Mode::Valid);
        auto backend = puls::makeBackendForTesting("speedtest", localBackendOptions(QUrl("http://127.0.0.1")));
        auto config = measureConfig("speedtest", "ping");
        config.server = QStringLiteral("127.0.0.1:%1").arg(server.port());
        const puls::Cancellation cancellation;
        (void)backend->select(config, cancellation);
        const auto phase = backend->ping(config, cancellation);
        expect(phase.value("status") == "ok" && phase.value("samples").toInt() == 10,
               "QMS local WebSocket completes valid ten-PONG handshake");
        expect(server.pingCount() == 10, "QMS sends exactly ten ping commands");
        expect(phase.value("method") == "median", "QMS ping uses median as the primary statistic");
    }
    for (const auto mode : {QmsMockServer::Mode::MalformedText, QmsMockServer::Mode::Binary}) {
        QmsMockServer server(mode);
        auto backend = puls::makeBackendForTesting("speedtest", localBackendOptions(QUrl("http://127.0.0.1")));
        auto config = measureConfig("speedtest", "ping");
        config.server = QStringLiteral("127.0.0.1:%1").arg(server.port());
        const puls::Cancellation cancellation;
        (void)backend->select(config, cancellation);
        const auto error = captureOpError([&] { (void)backend->ping(config, cancellation); });
        expect(error && error->code == "protocol" && !error->retryable,
               "malformed text and binary QMS frames are non-retryable protocol errors");
    }
    {
        QmsMockServer server(QmsMockServer::Mode::EarlyDisconnect);
        auto backend = puls::makeBackendForTesting("speedtest", localBackendOptions(QUrl("http://127.0.0.1")));
        auto config = measureConfig("speedtest", "ping");
        config.server = QStringLiteral("127.0.0.1:%1").arg(server.port());
        const puls::Cancellation cancellation;
        (void)backend->select(config, cancellation);
        const auto error = captureOpError([&] { (void)backend->ping(config, cancellation); });
        expect(error && error->code == "unavailable" && error->retryable,
               "early QMS disconnect is a retryable transport error");
    }
}

void testYandexWebSocketTimeoutOverrideIsOptional() {
    const QList<QJsonValue> values{QJsonValue(QJsonValue::Undefined), QJsonValue(QJsonValue::Null),
                                  QJsonValue(2000), QJsonValue(0), QJsonValue(-1),
                                  QJsonValue(30001), QJsonValue(1.5), QJsonValue("2000")};
    for (const auto& value : values) {
        QJsonObject payload;
        LocalHttpServer server([&](const QByteArray&) {
            return LocalHttpServer::Response{"200 OK", "application/json",
                QJsonDocument(payload).toJson(QJsonDocument::Compact)};
        });
        payload = yandexDiscovery(server.baseUrl().toString());
        auto upload = payload.value("upload").toObject();
        auto probes = upload.value("probes").toArray();
        auto item = probes.first().toObject();
        QUrl socketUrl = server.baseUrl();
        socketUrl.setScheme("ws");
        item.insert("websocketUrl", socketUrl.toString());
        item.insert("websocketConnectionTimeout", value);
        probes[0] = item;
        upload.insert("probes", probes);
        payload.insert("upload", upload);
        auto backend = puls::makeBackendForTesting("yandex", localBackendOptions(server.baseUrl()));
        const puls::Cancellation cancellation;
        const auto error = captureOpError([&] { (void)backend->select(measureConfig(), cancellation); });
        const bool accepted = value.isUndefined() || value.isNull() || value == QJsonValue(2000);
        expect(accepted ? !error.has_value() : error && error->code == "protocol",
               "Yandex accepts omitted timeout overrides and validates supplied integer bounds");
    }
}

void testCancellationHelper() {
    puls::Cancellation cancellation;
    puls::checkCanceled(cancellation, "ping");
    cancellation.cancel();
    expectThrows([&] { puls::checkCanceled(cancellation, "ping"); },
                 "cancellation helper throws after cancellation");
}

void testHumanRendererColorIsOptIn() {
    const QJsonObject phase{{"status", "ok"}, {"value_ms", 10.0}, {"jitter_ms", QJsonValue::Null}};
    const QJsonObject skipped{{"status", "skipped"}};
    const QJsonObject measurement{{"service", "yandex"}, {"status", "ok"},
                                  {"server", QJsonValue::Null},
                                  {"phases", QJsonObject{{"ping", phase}, {"download", skipped}, {"upload", skipped}}},
                                  {"warnings", QJsonArray{}}};
    const QJsonObject envelope{{"status", "ok"}, {"connection", QJsonValue::Null},
                               {"results", QJsonArray{measurement}}};
    const auto plain = puls::renderHuman(envelope);
    const auto colored = puls::renderHuman(envelope, true);
    expect(!plain.contains(QChar(0x1b)), "human renderer remains plain unless terminal color is requested");
    expect(colored.contains(QStringLiteral("\x1b[1mPuls · Яндекс.Интернетометр\x1b[0m")),
           "terminal renderer styles service heading");
    expect(colored.contains(QStringLiteral("\x1b[32m10.00 мс\x1b[0m")),
           "low latency uses the green latency threshold");
}

void testMeasurementExitCodes() {
    expect(puls::exitCode({{"status", "ok"}}) == 0, "successful envelope exit code");
    expect(puls::exitCode({{"status", "partial"}}) == 1, "partial envelope exit code");
    expect(puls::exitCode({{"status", "error"}}) == 1, "failed envelope exit code");
    expect(puls::exitCode({{"status", "canceled"}}) == 130, "canceled envelope exit code");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    testPublicErrorMessagesMatchSchemaOne();
    testParseMeasurementCommands();
    testBooleanFlagSyntax();
    testInvalidCLIArguments();
    testPingStatsParity();
    testHumanRendererColorIsOptIn();
    testCancellationHelper();
    testMeasurementExitCodes();
    testRunnerContinuesAllAfterServiceFailure();
    testRunnerDoesNotTurnPingFailureIntoZeroSuccess();
    testRunnerPreservesNullsAndEmptyArraysInSchemaOne();
    testConnectionFailureDoesNotChangeMeasurementAggregate();
    testIPDefaultFallbackAndExplicitServiceScope();
    testRunnerRunsOnlyRequestedPhase();
    testRunnerPreCanceledAndMidflightCancellation();
    testRunnerMarksOnlyRequestedPhasesCanceled();
    testRunnerAggregatesUniquePhaseWarnings();
    testYandexBackendDiscoversAgainstLoopbackMock();
    testRunnerRoutesNativeThroughputAfterDiscovery();
    testRunnerRoutesExplicitQmsThroughput();
    testYandexDiscoveryRejectsExternalInsecureProbeURL();
    testYandexConnectionParsesBoundedBootstrapIP();
    testSpeedtestConnectionUsesIPThenOptionalASNEndpoints();
    testSpeedtestRejectsISPOver255UTF8BytesButKeepsIP();
    testSpeedtestSelectsFasterLaterQmsServer();
    testDiscoveryRejectsWrongContentTypeAndOversizedBody();
    testDiscoveryAuthStatusClassificationAndKeyRefresh();
    testRealSocketCancellationClosesPendingDiscovery();
    testYandexCDNsCollectLatencyIndependently();
    testYandexWebSocketTimeoutOverrideIsOptional();
    testQmsLocalWebSocketHandshakeAndFailures();
    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return 1;
    }
    std::cout << "All Puls core regression tests passed\n";
    return 0;
}
