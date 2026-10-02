#include "puls/gui/controller.hpp"
#include "puls/gui/settings_store.hpp"

#include "support/fake_backend.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QPixmap>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickWindow>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariant>
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <ostream>
#include <utility>

// Readable QString values in test failure messages.
inline void PrintTo(const QString& value, std::ostream* output) {
    *output << '"' << value.toStdString() << '"';
}

namespace puls::gui {
namespace {

using namespace std::chrono_literals;
using service::ServiceId;
using testing::FakeBackend;

// Processes events until condition holds or the timeout expires.
bool wait_until(const std::function<bool()>& condition, std::chrono::milliseconds timeout = 5s) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeout.count()) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

struct Fixture {
    Fixture() : store(directory.filePath(QStringLiteral("puls.ini"))) {
        yandex->connection.external_ip = *IpAddress::parse("203.0.113.8");
        speedtest->connection.external_ip = *IpAddress::parse("203.0.113.7");
        speedtest->connection.isp = "Ростелеком";
    }

    app::RunnerOptions runner() {
        app::RunnerOptions options;
        options.yandex_factory = [this](const std::string&, const service::LogFunc&) {
            return std::shared_ptr<service::Backend>(yandex);
        };
        options.speedtest_factory = [this](const std::string&, const service::LogFunc&) {
            return std::shared_ptr<service::Backend>(speedtest);
        };
        return options;
    }

    std::unique_ptr<DashboardController> controller(const Context& root = Context()) {
        return std::make_unique<DashboardController>(root, QStringLiteral("1.2.3"), runner(),
                                                     store);
    }

    QTemporaryDir directory;
    SettingsStore store;
    std::shared_ptr<FakeBackend> yandex = std::make_shared<FakeBackend>(ServiceId::yandex);
    std::shared_ptr<FakeBackend> speedtest = std::make_shared<FakeBackend>(ServiceId::speedtest);
};

TEST(GuiController, RunsMeasurementOnWorkerThread) {
    Fixture fixture;
    auto controller = fixture.controller();
    EXPECT_EQ(controller->startLabel(), "Начать проверку");
    controller->toggleMeasurement();
    EXPECT_TRUE(controller->busy());
    EXPECT_TRUE(controller->measuring());
    EXPECT_EQ(controller->startLabel(), "Остановить");
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_EQ(controller->status(), "Проверка завершена");
    EXPECT_EQ(controller->statusTone(), static_cast<int>(Tone::success));
    EXPECT_TRUE(controller->hasResult());
    EXPECT_EQ(controller->ping(), "11");
    EXPECT_EQ(controller->jitter(), "1,5");
    EXPECT_EQ(controller->download(), "100");
    EXPECT_EQ(controller->upload(), "50,0");
    EXPECT_EQ(controller->heroValue(), "100");
    EXPECT_EQ(controller->heroUnit(), "Мбит/с");
    EXPECT_EQ(controller->heroLabel(), "Загрузка · Яндекс.Интернетометр");
    EXPECT_EQ(controller->activeMetric(), "");
    EXPECT_EQ(controller->serverText(), "mock.example · Владивосток");
    EXPECT_TRUE(controller->serverKnown());
    EXPECT_EQ(controller->startLabel(), "Проверить снова");
    const QVariantList phases = controller->phases();
    ASSERT_EQ(phases.size(), 3);
    for (const QVariant& phase : phases) {
        EXPECT_EQ(phase.toMap().value(QStringLiteral("state")).toString(), "done");
        EXPECT_EQ(phase.toMap().value(QStringLiteral("progress")).toDouble(), 1);
    }
    EXPECT_EQ(phases[1].toMap().value(QStringLiteral("title")).toString(), "Загрузка");
    // One service has no summary by service.
    EXPECT_TRUE(controller->results().isEmpty());
}

TEST(GuiController, SummarizesBothServices) {
    Fixture fixture;
    fixture.speedtest->upload_error = Error::make("обрыв соединения");
    auto controller = fixture.controller();
    controller->selectService(2);
    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_EQ(controller->status(), "Получен частичный результат");
    const QVariantList results = controller->results();
    ASSERT_EQ(results.size(), 2);
    const QVariantMap yandex = results[0].toMap();
    EXPECT_EQ(yandex.value(QStringLiteral("service")).toString(), "Яндекс.Интернетометр");
    EXPECT_EQ(yandex.value(QStringLiteral("status")).toString(), "готово");
    EXPECT_EQ(yandex.value(QStringLiteral("tone")).toInt(), static_cast<int>(Tone::success));
    EXPECT_EQ(yandex.value(QStringLiteral("ping")).toString(), "11 мс");
    EXPECT_EQ(yandex.value(QStringLiteral("download")).toString(), "100 Мбит/с");
    EXPECT_EQ(yandex.value(QStringLiteral("upload")).toString(), "50,0 Мбит/с");
    EXPECT_EQ(yandex.value(QStringLiteral("server")).toString(), "mock.example · Владивосток");
    const QVariantMap speedtest = results[1].toMap();
    EXPECT_EQ(speedtest.value(QStringLiteral("status")).toString(), "частично");
    EXPECT_EQ(speedtest.value(QStringLiteral("upload")).toString(), "ошибка");
    EXPECT_EQ(controller->heroLabel(), "Загрузка · speedtest.ru");
    EXPECT_TRUE(controller->notices().startsWith("Отдача (speedtest.ru): "));
    EXPECT_EQ(controller->noticeTone(), static_cast<int>(Tone::danger));
}

TEST(GuiController, CancelStopsActiveMeasurement) {
    Fixture fixture;
    fixture.yandex->block_download = true;
    auto controller = fixture.controller();
    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return controller->heroValue() == "80,0"; }));
    EXPECT_EQ(controller->activeMetric(), "download");
    EXPECT_EQ(controller->download(), "80,0");
    controller->toggleMeasurement();
    EXPECT_TRUE(controller->stopping());
    EXPECT_EQ(controller->startLabel(), "Останавливаем…");
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_FALSE(controller->stopping());
    EXPECT_EQ(controller->status(), "Проверка остановлена");
    EXPECT_EQ(controller->statusTone(), static_cast<int>(Tone::warning));
    // The live value of a stopped phase is not a result.
    EXPECT_EQ(controller->download(), "—");
    EXPECT_EQ(controller->heroLabel(), "Задержка · Яндекс.Интернетометр");
}

TEST(GuiController, RootCancellationStopsMeasurement) {
    Fixture fixture;
    fixture.yandex->block_download = true;
    CancelScope root{Context()};
    auto controller = fixture.controller(root.context());
    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return controller->heroValue() == "80,0"; }));
    root.cancel();
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_EQ(controller->status(), "Проверка остановлена");
}

TEST(GuiController, ShutdownCancelsAndJoinsWorker) {
    Fixture fixture;
    fixture.yandex->block_download = true;
    auto controller = fixture.controller();
    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return controller->heroValue() == "80,0"; }));
    QElapsedTimer timer;
    timer.start();
    controller.reset();
    EXPECT_LT(timer.elapsed(), 2000);
}

TEST(GuiController, DetectsConnectionWithFallback) {
    Fixture fixture;
    fixture.speedtest->connection_error = Error::make("speedtest недоступен");
    auto controller = fixture.controller();
    controller->selectService(2);
    controller->detectConnection();
    EXPECT_TRUE(controller->detecting());
    EXPECT_FALSE(controller->measuring());
    // A measurement waits until the lookup ends.
    controller->toggleMeasurement();
    EXPECT_FALSE(controller->measuring());
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_EQ(controller->connectionText(), "203.0.113.8");
    EXPECT_TRUE(controller->connectionKnown());
    EXPECT_EQ(controller->notices(), "использован резервный сервис Яндекс");
    EXPECT_EQ(controller->status(), "Готов к проверке");
    EXPECT_FALSE(controller->hasResult());
}

TEST(GuiController, SettingsAreValidatedAndOnlyPreferencesPersist) {
    Fixture fixture;
    auto controller = fixture.controller();
    int changes = 0;
    QObject::connect(controller.get(), &DashboardController::settingsChanged,
                     [&changes] { ++changes; });
    EXPECT_EQ(controller->applySettings(1, 0, QStringLiteral("2"), 4, 0, {}, false, 0),
              "Длительность должна быть от 3 до 60 секунд");
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(controller->applySettings(1, 0, QStringLiteral("8"), 4, 1,
                                        QStringLiteral("qms.example:443"), true, 2),
              "");
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(controller->serviceIndex(), 1);
    EXPECT_EQ(controller->speedtestServer(), "qms.example:443");
    EXPECT_TRUE(controller->showIp());
    EXPECT_EQ(controller->theme(), ThemeMode::dark);
    EXPECT_EQ(controller->themeLabel(), "Тёмное");
    EXPECT_EQ(controller->settingsSummary(), "Быстрый · 8 с · 4 соединения · только задержка");
    EXPECT_EQ(controller->idleHint(), "Измерим задержку через speedtest.ru.");
    EXPECT_EQ(controller->profileDurationSeconds(2), 15);
    controller->saveWindowSize(500, 700);

    QSettings settings(fixture.directory.filePath(QStringLiteral("puls.ini")),
                       QSettings::IniFormat);
    QStringList keys = settings.allKeys();
    keys.sort();
    EXPECT_EQ(keys, (QStringList{"connections", "duration_seconds", "phase", "profile", "service",
                                 "theme", "window_height", "window_width"}));
    EXPECT_EQ(settings.value(QStringLiteral("service")).toString(), "speedtest");

    auto reopened = fixture.controller();
    EXPECT_EQ(reopened->serviceIndex(), 1);
    EXPECT_EQ(reopened->durationSeconds(), 8);
    EXPECT_EQ(reopened->speedtestServer(), "");
    EXPECT_FALSE(reopened->showIp());
    EXPECT_EQ(reopened->windowWidth(), 500);
    EXPECT_EQ(reopened->minimumWindowWidth(), 390);
}

// Saves a screenshot when PULS_GUI_SCREENSHOTS names a directory.
void screenshot(QQuickWindow& window, const QString& name) {
    const QString directory = qEnvironmentVariable("PULS_GUI_SCREENSHOTS");
    if (directory.isEmpty()) {
        return;
    }
    // Lets the animations finish.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 500) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    window.grabWindow().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

// The dashboard window as it looks on the platform os, by default on the
// platform of the tests.
struct View {
    explicit View(DashboardController& controller, const QString& os = QStringLiteral("linux")) {
        QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                         [this](const QList<QQmlError>& messages) {
                             for (const QQmlError& message : messages) {
                                 warnings.append(message.toString());
                             }
                         });
        QVariantMap properties{{QStringLiteral("dashboard"), QVariant::fromValue(&controller)}};
        if (!os.isEmpty()) {
            properties.insert(QStringLiteral("os"), os);
        }
        engine.setInitialProperties(properties);
        engine.loadFromModule("Puls", "Main");
        if (!engine.rootObjects().isEmpty()) {
            window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
        }
    }

    [[nodiscard]] QObject* find(const char* name) const {
        return window->findChild<QObject*>(QString::fromLatin1(name));
    }
    [[nodiscard]] QVariant property(const char* object, const char* name) const {
        QObject* item = find(object);
        return item != nullptr ? item->property(name) : QVariant();
    }
    [[nodiscard]] std::string errors() const {
        return warnings.join(QLatin1Char('\n')).toStdString();
    }

    // Declared before the engine: QML may report warnings while it is destroyed.
    QStringList warnings;
    QQmlApplicationEngine engine;
    QQuickWindow* window = nullptr;
};

TEST(GuiQml, ShowsMeasurementWithoutWarnings) {
    Fixture fixture;
    auto controller = fixture.controller();
    View view(*controller);
    ASSERT_NE(view.window, nullptr) << view.errors();
    EXPECT_EQ(view.window->title(), "Puls");
    EXPECT_EQ(view.window->width(), 460);
    view.window->resize(460, 800);
    EXPECT_EQ(view.property("startButton", "text").toString(), "Начать проверку");
    screenshot(*view.window, QStringLiteral("idle-light"));

    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_EQ(view.property("heroNumber", "text").toString(), "100");
    EXPECT_EQ(view.property("startButton", "text").toString(), "Проверить снова");
    EXPECT_FALSE(view.property("results", "visible").toBool());
    controller->cycleTheme();
    controller->cycleTheme();
    QCoreApplication::processEvents();
    EXPECT_TRUE(view.window->property("dark").toBool());
    screenshot(*view.window, QStringLiteral("result-dark"));
    view.window->resize(1000, 760);
    screenshot(*view.window, QStringLiteral("result-dark-wide"));

    controller->selectService(2);
    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_TRUE(view.property("results", "visible").toBool());
    screenshot(*view.window, QStringLiteral("both-dark-wide"));
    EXPECT_TRUE(view.warnings.isEmpty()) << view.errors();
}

TEST(GuiQml, SettingsPanelSavesAndReportsErrors) {
    Fixture fixture;
    auto controller = fixture.controller();
    View view(*controller);
    ASSERT_NE(view.window, nullptr) << view.errors();
    view.window->resize(460, 800);
    QObject* panel = view.find("settingsPanel");
    ASSERT_NE(panel, nullptr);

    QMetaObject::invokeMethod(view.window, "openSettings");
    ASSERT_TRUE(wait_until([&] { return panel->property("opened").toBool(); }));
    EXPECT_EQ(view.property("durationSlider", "value").toInt(), 10);
    EXPECT_EQ(view.property("profileChoice", "currentIndex").toInt(), 1);
    EXPECT_FALSE(view.property("serverField", "enabled").toBool());
    screenshot(*view.window, QStringLiteral("settings-light"));

    panel->setProperty("duration", 2);
    QMetaObject::invokeMethod(panel, "save");
    EXPECT_EQ(panel->property("error").toString(), "Длительность должна быть от 3 до 60 секунд");
    EXPECT_TRUE(view.property("settingsError", "visible").toBool());
    EXPECT_TRUE(panel->property("opened").toBool());
    EXPECT_EQ(controller->durationSeconds(), 10);
    screenshot(*view.window, QStringLiteral("settings-error"));

    panel->setProperty("profile", 2);
    panel->setProperty("duration", 15);
    panel->setProperty("theme", 2);
    QMetaObject::invokeMethod(panel, "save");
    EXPECT_FALSE(panel->property("opened").toBool());
    EXPECT_EQ(controller->profileIndex(), 2);
    EXPECT_EQ(controller->durationSeconds(), 15);
    EXPECT_EQ(controller->themeIndex(), 2);
    EXPECT_EQ(controller->settingsSummary(), "Точный · 15 с");

    // Settings do not open during a measurement.
    fixture.yandex->block_download = true;
    controller->toggleMeasurement();
    QMetaObject::invokeMethod(view.window, "openSettings");
    EXPECT_FALSE(panel->property("opened").toBool());
    EXPECT_FALSE(view.property("settingsButton", "enabled").toBool());
    controller->cancel();
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_TRUE(view.warnings.isEmpty()) << view.errors();
}

TEST(GuiQml, ShowsErrorsAsToast) {
    Fixture fixture;
    auto controller = fixture.controller();
    View view(*controller);
    ASSERT_NE(view.window, nullptr) << view.errors();
    EXPECT_FALSE(view.property("toast", "shown").toBool());
    emit controller->errorOccurred(QStringLiteral("нет подключения к сети"));
    EXPECT_TRUE(view.property("toast", "shown").toBool());
    EXPECT_EQ(view.property("toast", "text").toString(), "нет подключения к сети");
    EXPECT_TRUE(view.warnings.isEmpty()) << view.errors();
}

TEST(GuiQml, TitleBarFollowsPlatform) {
    Fixture fixture;
    auto controller = fixture.controller();
    {
        View view(*controller, QStringLiteral("linux"));
        ASSERT_NE(view.window, nullptr) << view.errors();
        EXPECT_TRUE(view.window->flags().testFlag(Qt::FramelessWindowHint));
        EXPECT_TRUE(view.property("minimizeButton", "visible").toBool());
        EXPECT_TRUE(view.property("closeButton", "visible").toBool());
        EXPECT_TRUE(view.warnings.isEmpty()) << view.errors();
    }
    {
        View view(*controller, QStringLiteral("windows"));
        ASSERT_NE(view.window, nullptr) << view.errors();
        const Qt::WindowFlags flags = view.window->flags();
        EXPECT_TRUE(flags.testFlag(Qt::ExpandedClientAreaHint));
        EXPECT_TRUE(flags.testFlag(Qt::NoTitleBarBackgroundHint));
        // Without the title hint Windows draws no title and icon of its own.
        EXPECT_TRUE(flags.testFlag(Qt::CustomizeWindowHint));
        EXPECT_FALSE(flags.testFlag(Qt::WindowTitleHint));
        EXPECT_TRUE(flags.testFlag(Qt::WindowCloseButtonHint));
        EXPECT_FALSE(view.property("minimizeButton", "visible").toBool());
        // Room for the three system buttons, each 1.5 bar heights wide.
        EXPECT_EQ(view.property("titleBar", "trailingInset").toInt(), 144);
        EXPECT_EQ(view.window->title(), "Puls");
        EXPECT_TRUE(view.warnings.isEmpty()) << view.errors();
    }
    {
        View view(*controller, QStringLiteral("osx"));
        ASSERT_NE(view.window, nullptr) << view.errors();
        EXPECT_TRUE(view.window->flags().testFlag(Qt::ExpandedClientAreaHint));
        EXPECT_FALSE(view.window->flags().testFlag(Qt::CustomizeWindowHint));
        // macOS would draw the title over the bar.
        EXPECT_EQ(view.window->title(), "");
        EXPECT_EQ(view.property("titleBar", "leadingInset").toInt(), 72);
        EXPECT_FALSE(view.property("closeButton", "visible").toBool());
        EXPECT_TRUE(view.warnings.isEmpty()) << view.errors();
    }
    {
        View view(*controller, QStringLiteral("android"));
        ASSERT_NE(view.window, nullptr) << view.errors();
        EXPECT_TRUE(view.window->flags().testFlag(Qt::ExpandedClientAreaHint));
        EXPECT_EQ(view.property("titleBar", "height").toInt(), 56);
        EXPECT_FALSE(view.property("closeButton", "visible").toBool());
        EXPECT_TRUE(view.warnings.isEmpty()) << view.errors();
    }
}

// Captures the window together with what the system draws, such as the
// window buttons on Windows and macOS, when PULS_GUI_NATIVE_SCREENSHOTS
// names a directory. CI runs it with the platform plugin of the system.
TEST(GuiQml, NativeWindowScreenshots) {
    const QString directory = qEnvironmentVariable("PULS_GUI_NATIVE_SCREENSHOTS");
    if (directory.isEmpty() || QGuiApplication::platformName() == QLatin1String("offscreen")) {
        GTEST_SKIP() << "PULS_GUI_NATIVE_SCREENSHOTS is empty or the platform is offscreen";
    }
    Fixture fixture;
    auto controller = fixture.controller();
    follow_system_theme(*controller);
    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    for (const auto& [theme, name] : {std::pair{1, "native-light"}, std::pair{2, "native-dark"}}) {
        while (controller->themeIndex() != theme) {
            controller->cycleTheme();
        }
        View view(*controller, QString());
        ASSERT_NE(view.window, nullptr) << view.errors();
        view.window->setPosition(64, 64);
        // Lets the system show the window and the animations finish.
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 2000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        QScreen* screen = view.window->screen();
        ASSERT_NE(screen, nullptr);
        const QRect area = view.window->frameGeometry()
                               .adjusted(-24, -24, 24, 24)
                               .translated(-screen->geometry().topLeft());
        const QPixmap pixmap =
            screen->grabWindow(0, area.x(), area.y(), area.width(), area.height());
        ASSERT_FALSE(pixmap.isNull());
        EXPECT_TRUE(pixmap.save(QDir(directory).filePath(QString::fromLatin1(name) + ".png")));
        EXPECT_TRUE(view.warnings.isEmpty()) << view.errors();
    }
}

} // namespace
} // namespace puls::gui

// Qt keeps process-wide scene graph, pixmap and font caches until exit, the
// font caches partly in fontconfig; LeakSanitizer reports them when the tests
// run with sanitizers.
extern "C" const char* __lsan_default_suppressions();
extern "C" const char* __lsan_default_suppressions() {
    return "leak:libQt6\nleak:libfontconfig\n";
}

int main(int argc, char** argv) {
    // Tests run without a display; the software renderer needs no GPU.
    const auto set_default = [](const char* name, const char* value) {
        if (qEnvironmentVariableIsEmpty(name)) {
            qputenv(name, value);
        }
    };
    set_default("QT_QPA_PLATFORM", "offscreen");
    if (qgetenv("QT_QPA_PLATFORM") == "offscreen") {
        set_default("QT_QUICK_BACKEND", "software");
    }
    QGuiApplication application(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
