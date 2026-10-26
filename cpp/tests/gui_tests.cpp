#include "puls/gui.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyleFactory>
#include <QTemporaryDir>
#include <QThread>
#include <atomic>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
int failures = 0;

void expect(bool condition, const char* message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

bool until(const std::function<bool()>& condition, int timeout = 1500) {
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 10);
        if (condition()) return true;
        QThread::msleep(1);
    } while (elapsed.elapsed() < timeout);
    QApplication::processEvents();
    return condition();
}

void settle() {
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
}

template <typename Widget>
Widget* control(QWidget* window, const char* name) {
    auto* widget = window->findChild<Widget*>(QString::fromLatin1(name));
    if (!widget) throw std::runtime_error(std::string("Missing GUI control: ") + name);
    return widget;
}

struct State {
    bool block = false;
    bool fail = false;
    bool blockSelection = false;
    QThread* guiThread = nullptr;
    std::atomic_int created{0};
    std::atomic_int destroyed{0};
    std::atomic_int entered{0};
    std::atomic_int selected{0};
    std::atomic_int canceled{0};
    std::atomic_int wrongThread{0};
};

class FakeBackend final : public puls::Backend {
public:
    FakeBackend(QString service, std::shared_ptr<State> state)
        : service_(std::move(service)), state_(std::move(state)), owner_(QThread::currentThread()) {
        state_->created.fetch_add(1);
        if (owner_ == state_->guiThread) state_->wrongThread.fetch_add(1);
    }
    ~FakeBackend() override {
        audit();
        state_->destroyed.fetch_add(1);
    }
    QString id() const override { return service_; }

    QJsonObject select(const puls::Config&, const puls::Cancellation& cancellation) override {
        audit();
        state_->selected.fetch_add(1);
        if (state_->blockSelection) {
            QElapsedTimer elapsed;
            elapsed.start();
            while (!cancellation.canceled() && elapsed.elapsed() < 3000) QThread::msleep(1);
            if (cancellation.canceled()) state_->canceled.fetch_add(1);
        }
        puls::checkCanceled(cancellation, "select");
        return {{"name", "Synthetic server that must stay in memory"},
                {"city", QStringLiteral("Владивосток")}, {"region", QJsonValue::Null}};
    }

    QJsonObject ping(const puls::Config&, const puls::Cancellation& cancellation) override {
        audit();
        state_->entered.fetch_add(1);
        if (state_->block) {
            QElapsedTimer elapsed;
            elapsed.start();
            while (!cancellation.canceled() && elapsed.elapsed() < 3000) QThread::msleep(1);
            if (cancellation.canceled()) state_->canceled.fetch_add(1);
            puls::checkCanceled(cancellation, "ping");
        }
        if (state_->fail) throw puls::OpError("network", "ping", QStringLiteral(
            "Сервер измерения временно недоступен. Проверьте подключение к интернету и повторите проверку. "
            "Это длинное диагностическое сообщение проверяет перенос строк при уменьшении окна."));
        return puls::pingPhase({12.34, 12.34, 12.34}, "minimum");
    }

    QJsonObject download(const puls::Config&, const puls::Cancellation& cancellation) override {
        audit(); puls::checkCanceled(cancellation, "download");
        auto phase = puls::emptyPhase("ok");
        phase.insert("mbps", 420.5);
        return phase;
    }

    QJsonObject upload(const puls::Config&, const puls::Cancellation& cancellation) override {
        audit(); puls::checkCanceled(cancellation, "upload");
        auto phase = puls::emptyPhase("ok");
        phase.insert("mbps", 95.25);
        return phase;
    }

    QJsonObject connection(const puls::Cancellation& cancellation) override {
        audit(); puls::checkCanceled(cancellation, "connection");
        return {{"status", "ok"}, {"external_ip", "203.0.113.7"},
                {"isp", "Synthetic ISP that must stay in memory"}, {"warnings", QJsonArray{}}};
    }

private:
    QString service_;
    std::shared_ptr<State> state_;
    QThread* owner_;
    void audit() const {
        if (QThread::currentThread() != owner_ || QThread::currentThread() == state_->guiThread)
            state_->wrongThread.fetch_add(1);
    }
};

class ThreadAudit final : public QObject {
public:
    explicit ThreadAudit(QThread* guiThread) : guiThread_(guiThread) {}
    int wrongThread = 0;
protected:
    bool eventFilter(QObject*, QEvent*) override {
        if (QThread::currentThread() != guiThread_) ++wrongThread;
        return false;
    }
private:
    QThread* guiThread_;
};

puls::BackendFactory factory(const std::shared_ptr<State>& state) {
    return [state](const QString& service) { return std::make_unique<FakeBackend>(service, state); };
}

puls::Config config(const QString& service = "yandex") {
    puls::Config result;
    result.service = service;
    result.only = "ping";
    return result;
}

std::shared_ptr<State> state() {
    auto result = std::make_shared<State>();
    result->guiThread = qApp->thread();
    return result;
}

void clearPreferences() {
    QSettings preferences(QSettings::IniFormat, QSettings::UserScope, "Puls", "Puls");
    preferences.setFallbacksEnabled(false);
    preferences.clear();
    preferences.sync();
}

void assertControlsVisible(QWidget* window);

void testSuccessAndAffinity() {
    clearPreferences();
    const auto backend = state();
    auto request = config();
    request.only = "all";
    std::unique_ptr<QWidget> window(puls::createMainWindow(request, factory(backend)));
    window->show();
    settle();
    ThreadAudit audit(qApp->thread());
    for (auto* widget : window->findChildren<QWidget*>()) widget->installEventFilter(&audit);
    auto* start = control<QPushButton>(window.get(), "startButton");
    start->click();
    expect(until([&] {
        return backend->destroyed.load() == 1 && start->isEnabled() && start->text() == QStringLiteral("Начать проверку");
    }), "successful run completes and returns action to start");
    expect(control<QLabel>(window.get(), "result_yandex_ping")->text() == QStringLiteral("12.34 мс"), "ping result reaches the GUI");
    expect(control<QLabel>(window.get(), "result_yandex_download")->text() == QStringLiteral("420.50 Мбит/с"), "download result reaches the GUI");
    expect(control<QLabel>(window.get(), "result_yandex_upload")->text() == QStringLiteral("95.25 Мбит/с"), "upload result reaches the GUI");
    expect(control<QLabel>(window.get(), "statusLabel")->text() == QStringLiteral("Проверка завершена"), "success status reaches the GUI");
    expect(backend->created.load() == 1 && backend->wrongThread.load() == 0, "backend lifetime and calls stay in one non-GUI worker thread");
    expect(audit.wrongThread == 0, "widget event dispatch stays on the GUI thread");
    for (const auto& size : QList<QSize>{{360, 294}, {360, 640}, {900, 600}, {1280, 800}, {360, 294}}) {
        window->resize(size);
        settle();
        assertControlsVisible(window.get());
    }
    window->close();
    settle();
}

void testStopAndRestart() {
    clearPreferences();
    const auto backend = state();
    backend->block = true;
    std::unique_ptr<QWidget> window(puls::createMainWindow(config(), factory(backend)));
    window->show();
    settle();
    auto* start = control<QPushButton>(window.get(), "startButton");
    for (int run = 1; run <= 2; ++run) {
        start->click();
        expect(until([&] { return backend->entered.load() == run; }), "active fake starts in worker thread");
        expect(start->text() == QStringLiteral("Остановить"), "active run exposes stop action");
        QElapsedTimer elapsed;
        elapsed.start();
        start->click();
        expect(until([&] { return backend->destroyed.load() == run && start->isEnabled(); }, 1000), "stop cancels and cleans worker within one second");
        expect(elapsed.elapsed() < 1000 && backend->canceled.load() == run, "stop reaches active backend cancellation token");
        expect(control<QLabel>(window.get(), "statusLabel")->text() == QStringLiteral("Проверка остановлена"), "canceled result is rendered as canceled");
        expect(control<QLabel>(window.get(), "result_yandex_ping")->text() == QStringLiteral("Остановлено"), "cancellation never becomes successful zero throughput");
    }
    expect(backend->wrongThread.load() == 0, "restart keeps each backend in its worker thread");
    window->close();
    settle();
}

void testCloseAndDestructionCancel() {
    clearPreferences();
    const auto backend = state();
    backend->block = true;
    std::unique_ptr<QWidget> window(puls::createMainWindow(config(), factory(backend)));
    window->show();
    settle();
    control<QPushButton>(window.get(), "startButton")->click();
    expect(until([&] { return backend->entered.load() == 1; }), "close test has active backend");
    QElapsedTimer elapsed;
    elapsed.start();
    window->close();
    expect(until([&] { return !window->isVisible() && backend->destroyed.load() == 1; }, 1000), "close cancels active run before hiding window");
    expect(elapsed.elapsed() < 1000 && backend->canceled.load() == 1, "close cancellation completes within one second");
    window.reset();
    settle();

    const auto destroyedBackend = state();
    destroyedBackend->block = true;
    window.reset(puls::createMainWindow(config(), factory(destroyedBackend)));
    window->show();
    settle();
    control<QPushButton>(window.get(), "startButton")->click();
    expect(until([&] { return destroyedBackend->entered.load() == 1; }), "destructor test has active backend");
    elapsed.restart();
    window.reset();
    expect(elapsed.elapsed() < 1000, "destructor cancels active backend without long blocking");
    expect(destroyedBackend->destroyed.load() == 1 && destroyedBackend->canceled.load() == 1, "destructor joins and destroys active backend");
    settle();
}

void testPrivatePreferences() {
    clearPreferences();
    const auto backend = state();
    auto request = config();
    request.showIP = true;
    std::unique_ptr<QWidget> window(puls::createMainWindow(request, factory(backend)));
    window->show();
    settle();
    auto* start = control<QPushButton>(window.get(), "startButton");
    start->click();
    expect(until([&] { return backend->destroyed.load() == 2 && start->text() == QStringLiteral("Начать проверку"); }), "connection lookup and measurement complete");
    expect(control<QLabel>(window.get(), "connectionLabel")->text().contains("203.0.113.7"), "connection data can be rendered in memory");
    control<QSpinBox>(window.get(), "durationSpin")->setValue(12);
    control<QSpinBox>(window.get(), "connectionsSpin")->setValue(3);
    control<QComboBox>(window.get(), "themeChoice")->setCurrentIndex(2);
    window->resize(900, 600);
    window->close();
    settle();
    QSettings preferences(QSettings::IniFormat, QSettings::UserScope, "Puls", "Puls");
    preferences.setFallbacksEnabled(false);
    preferences.sync();
    const QStringList allowed{"theme", "service", "profile", "duration", "connections", "phase", "windowSize"};
    const auto keys = preferences.allKeys();
    expect(!keys.isEmpty(), "GUI stores allowed preferences");
    for (const auto& key : keys) expect(allowed.contains(key), "GUI persists only whitelisted preference keys");
    expect(preferences.value("duration").toInt() == 12 && preferences.value("connections").toInt() == 3, "measurement preferences persist");
    QFile file(preferences.fileName());
    expect(file.open(QIODevice::ReadOnly), "preferences file can be inspected");
    const auto contents = file.readAll();
    expect(!contents.contains("203.0.113.7") && !contents.contains("Synthetic ISP") && !contents.contains("Synthetic server"), "preferences contain no IP, ISP, server or result text");
}

void testServiceNavigationAndDiscoveryProgress() {
    clearPreferences();
    const auto backend = state();
    std::unique_ptr<QWidget> window(puls::createMainWindow(config(), factory(backend)));
    window->show();
    settle();
    control<QPushButton>(window.get(), "serviceSegment_2")->click();
    settle();
    expect(control<QLabel>(window.get(), "result_speedtest_ping")->isVisible(), "inline both-services selector exposes both result panels");
    auto* summary = control<QPushButton>(window.get(), "summaryButton");
    summary->click();
    expect(control<QWidget>(window.get(), "settingsPage")->isVisible(), "parameter summary opens the separate settings page");
    control<QPushButton>(window.get(), "backButton")->click();
    expect(control<QWidget>(window.get(), "measurePage")->isVisible(), "settings back action returns to the dashboard");
    auto* start = control<QPushButton>(window.get(), "startButton");
    start->click();
    expect(until([&] { return backend->destroyed.load() == 2 && start->text() == QStringLiteral("Начать проверку"); }), "inline selection runs both services through Runner");
    expect(control<QLabel>(window.get(), "result_yandex_ping")->text() == QStringLiteral("12.34 мс") &&
           control<QLabel>(window.get(), "result_speedtest_ping")->text() == QStringLiteral("12.34 мс"), "both selected services populate separate results");
    window->close();
    window.reset();
    settle();

    const auto discovery = state();
    discovery->blockSelection = true;
    window.reset(puls::createMainWindow(config(), factory(discovery)));
    window->show();
    settle();
    start = control<QPushButton>(window.get(), "startButton");
    start->click();
    expect(until([&] { return discovery->selected.load() == 1; }), "discovery runs asynchronously");
    auto* progress = control<QProgressBar>(window.get(), "phaseProgress");
    expect(until([&] { return progress->isVisible() && progress->minimum() == 0 && progress->maximum() == 0; }), "server discovery exposes indeterminate progress");
    expect(!control<QPushButton>(window.get(), "serviceSegment_0")->isEnabled() &&
           !control<QPushButton>(window.get(), "summaryButton")->isEnabled(), "active run protects service and parameter controls");
    start->click();
    expect(until([&] { return discovery->destroyed.load() == 1 && start->isEnabled(); }, 1000), "discovery can be canceled through the pinned action");
    expect(!progress->isVisible() && control<QPushButton>(window.get(), "summaryButton")->isEnabled(), "completion removes progress and restores parameter access");
    window->close();
    settle();
}

void assertControlsVisible(QWidget* window) {
    auto* start = control<QPushButton>(window, "startButton");
    auto* menu = control<QPushButton>(window, "menuButton");
    const QRect startRect(start->mapTo(window, QPoint()), start->size());
    const QRect menuRect(menu->mapTo(window, QPoint()), menu->size());
    expect(start->isVisible() && window->rect().contains(startRect), "start/stop control remains fully visible after resize");
    expect(menu->isVisible() && window->rect().contains(menuRect), "menu control remains fully visible after resize");
    expect(start->height() >= 44, "resizing preserves accessible action size");
    auto* summaryButton = control<QPushButton>(window, "summaryButton");
    auto* summary = control<QLabel>(window, "settingsSummary");
    const QRect summaryRect(summary->mapTo(summaryButton, QPoint()), summary->size());
    expect(summaryButton->rect().contains(summaryRect) && summary->height() >= summary->heightForWidth(summary->width()),
           "parameter summary remains fully readable inside its clickable row");
    for (auto* value : window->findChildren<QLabel*>()) {
        if (!value->objectName().startsWith("result_")) continue;
        expect(value->height() >= value->heightForWidth(value->width()), "metric values have enough height without shrinking the font");
    }
    auto* scroll = control<QScrollArea>(window, "resultsScroll");
    if (scroll->widget()->width() > scroll->viewport()->width())
        std::cerr << "Layout extent: window " << window->width() << 'x' << window->height()
                  << ", content " << scroll->widget()->width() << ", viewport " << scroll->viewport()->width()
                  << ", content minimum " << scroll->widget()->minimumSizeHint().width() << '\n';
    expect(scroll->widget()->width() <= scroll->viewport()->width(), "result content fits available width without horizontal clipping");
}

void testResponsiveErrorsAndThemes() {
    clearPreferences();
    const auto backend = state();
    backend->fail = true;
    std::unique_ptr<QWidget> window(puls::createMainWindow(config("all"), factory(backend)));
    window->show();
    settle();
    auto* start = control<QPushButton>(window.get(), "startButton");
    start->click();
    expect(until([&] { return backend->destroyed.load() == 2 && start->text() == QStringLiteral("Начать проверку"); }), "both service failures reach the dashboard");
    auto* themes = control<QComboBox>(window.get(), "themeChoice");
    for (int theme = 0; theme < themes->count(); ++theme) {
        themes->setCurrentIndex(theme);
        for (const auto& size : QList<QSize>{{360, 294}, {360, 640}, {900, 600}, {1280, 800}, {360, 294}}) {
            window->resize(size);
            settle();
            expect(window->size() == size, "dashboard accepts each required viewport size");
            assertControlsVisible(window.get());
            for (const auto* name : {"error_yandex", "error_speedtest"}) {
                auto* error = control<QLabel>(window.get(), name);
                expect(error->isVisible() && error->wordWrap(), "long errors stay visible and wrap across themes and sizes");
                expect(error->height() >= error->heightForWidth(error->width()), "long error label has enough height for its current width");
            }
            auto* scroll = control<QScrollArea>(window.get(), "resultsScroll");
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            settle();
            assertControlsVisible(window.get());
            if (size == QSize(360, 294)) expect(scroll->verticalScrollBar()->maximum() > 0, "compact error view exposes vertical scrolling");
        }
    }
    window->close();
    settle();
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    auto font = application.font();
    font.setPointSize(11);
    application.setFont(font);
    qunsetenv("PULS_GUI_PREVIEW_DIR");
    const auto scratchRoot = QDir::currentPath() + "/.scratch";
    if (!QDir().mkpath(scratchRoot)) return 1;
    QTemporaryDir scratch(scratchRoot + "/gui-tests-XXXXXX");
    if (!scratch.isValid()) return 1;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch.path());
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, scratch.path());
    try {
        testSuccessAndAffinity();
        testStopAndRestart();
        testCloseAndDestructionCancel();
        testPrivatePreferences();
        testServiceNavigationAndDiscoveryProgress();
        testResponsiveErrorsAndThemes();
    } catch (const std::exception& error) {
        ++failures;
        std::cerr << "FAIL: GUI test exception: " << error.what() << '\n';
    }
    if (failures == 0) std::cout << "GUI contract checks passed\n";
    return failures == 0 ? 0 : 1;
}
