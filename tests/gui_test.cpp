#include "puls/gui/controller.hpp"
#include "puls/gui/settings_store.hpp"

#include "support/fake_backend.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariant>
#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <ostream>

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
    EXPECT_EQ(controller->startLabel(), "Остановить");
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_EQ(controller->status(), "Проверка завершена");
    EXPECT_EQ(controller->statusTone(), static_cast<int>(Tone::success));
    EXPECT_EQ(controller->ping(), "11,00");
    EXPECT_EQ(controller->jitter(), "1,50");
    EXPECT_EQ(controller->download(), "100,00");
    EXPECT_EQ(controller->upload(), "50,00");
    EXPECT_EQ(controller->serverText(), "mock.example · Владивосток");
    EXPECT_FALSE(controller->progressVisible());
    EXPECT_EQ(controller->startLabel(), "Проверить снова");
    const QVariantList results = controller->results();
    ASSERT_EQ(results.size(), 1);
    EXPECT_EQ(results[0].toMap().value(QStringLiteral("title")).toString(),
              "Яндекс.Интернетометр · готово");
}

TEST(GuiController, CancelStopsActiveMeasurement) {
    Fixture fixture;
    fixture.yandex->block_download = true;
    auto controller = fixture.controller();
    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return controller->currentValue() == "80,00"; }));
    controller->toggleMeasurement();
    EXPECT_TRUE(controller->stopping());
    EXPECT_EQ(controller->startLabel(), "Останавливаем…");
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_EQ(controller->status(), "Проверка остановлена");
    EXPECT_EQ(controller->statusTone(), static_cast<int>(Tone::warning));
}

TEST(GuiController, RootCancellationStopsMeasurement) {
    Fixture fixture;
    fixture.yandex->block_download = true;
    CancelScope root{Context()};
    auto controller = fixture.controller(root.context());
    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return controller->currentValue() == "80,00"; }));
    root.cancel();
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_EQ(controller->status(), "Проверка остановлена");
}

TEST(GuiController, ShutdownCancelsAndJoinsWorker) {
    Fixture fixture;
    fixture.yandex->block_download = true;
    auto controller = fixture.controller();
    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return controller->currentValue() == "80,00"; }));
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
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    EXPECT_EQ(controller->status(), "Подключение определено");
    EXPECT_EQ(controller->connectionText(), "203.0.113.8 · через Яндекс.Интернетометр");
    EXPECT_EQ(controller->notices(), "использован резервный сервис Яндекс");
}

TEST(GuiController, SettingsAreValidatedAndOnlyPreferencesPersist) {
    Fixture fixture;
    auto controller = fixture.controller();
    int changes = 0;
    QObject::connect(controller.get(), &DashboardController::settingsChanged,
                     [&changes] { ++changes; });
    EXPECT_EQ(controller->applySettings(1, 0, QStringLiteral("2"), 4, 0, {}, false, 0),
              "длительность должна быть от 3 до 60 секунд");
    EXPECT_EQ(changes, 0);
    EXPECT_EQ(controller->applySettings(1, 0, QStringLiteral("8"), 4, 1,
                                        QStringLiteral("qms.example:443"), true, 2),
              "");
    EXPECT_EQ(changes, 1);
    EXPECT_EQ(controller->serviceIndex(), 1);
    EXPECT_EQ(controller->speedtestServer(), "qms.example:443");
    EXPECT_TRUE(controller->showIp());
    EXPECT_EQ(controller->themeLabel(), "Тёмное");
    EXPECT_EQ(controller->settingsSummary(), "Быстрый  ·  8 с  ·  4  ·  Только задержка");
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
}

// Saves a screenshot when PULS_GUI_SCREENSHOTS names a directory.
void screenshot(QQuickWindow& window, const QString& name) {
    const QString directory = qEnvironmentVariable("PULS_GUI_SCREENSHOTS");
    if (directory.isEmpty()) {
        return;
    }
    QCoreApplication::processEvents();
    window.grabWindow().save(QDir(directory).filePath(name + QStringLiteral(".png")));
}

TEST(GuiQml, LoadsDashboardWithoutWarnings) {
    Fixture fixture;
    auto controller = fixture.controller();
    QStringList warnings;
    // Declared after the controller so that QML objects are destroyed first.
    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                     [&warnings](const QList<QQmlError>& errors) {
                         for (const QQmlError& error : errors) {
                             warnings.append(error.toString());
                         }
                     });
    engine.setInitialProperties(
        {{QStringLiteral("dashboard"), QVariant::fromValue(controller.get())}});
    engine.load(QUrl(QStringLiteral("qrc:/puls/qml/Main.qml")));
    ASSERT_EQ(engine.rootObjects().size(), 1) << warnings.join(QLatin1Char('\n')).toStdString();
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    ASSERT_NE(window, nullptr);
    EXPECT_EQ(window->title(), "Puls");
    EXPECT_EQ(window->width(), 460);
    screenshot(*window, QStringLiteral("idle-light"));

    controller->toggleMeasurement();
    ASSERT_TRUE(wait_until([&] { return !controller->busy(); }));
    controller->cycleTheme();
    controller->cycleTheme();
    QCoreApplication::processEvents();
    screenshot(*window, QStringLiteral("result-dark"));
    window->resize(820, 900);
    QCoreApplication::processEvents();
    screenshot(*window, QStringLiteral("result-dark-wide"));
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();
}

TEST(GuiQml, SettingsDialogReportsInvalidInput) {
    Fixture fixture;
    auto controller = fixture.controller();
    QQmlApplicationEngine engine;
    engine.setInitialProperties(
        {{QStringLiteral("dashboard"), QVariant::fromValue(controller.get())}});
    engine.load(QUrl(QStringLiteral("qrc:/puls/qml/Main.qml")));
    ASSERT_EQ(engine.rootObjects().size(), 1);
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    ASSERT_NE(window, nullptr);
    QObject* dialog = window->findChild<QObject*>(QStringLiteral("settingsDialog"));
    QObject* errors = window->findChild<QObject*>(QStringLiteral("errorDialog"));
    ASSERT_NE(dialog, nullptr);
    ASSERT_NE(errors, nullptr);

    QMetaObject::invokeMethod(dialog, "open");
    ASSERT_TRUE(wait_until([&] { return dialog->property("opened").toBool(); }));
    QObject* duration = window->findChild<QObject*>(QStringLiteral("durationField"));
    ASSERT_NE(duration, nullptr);
    EXPECT_EQ(duration->property("text").toString(), "10");
    screenshot(*window, QStringLiteral("settings-light"));

    duration->setProperty("text", QStringLiteral("2"));
    QObject* save = window->findChild<QObject*>(QStringLiteral("saveButton"));
    ASSERT_NE(save, nullptr);
    QMetaObject::invokeMethod(save, "clicked");
    ASSERT_TRUE(wait_until([&] { return errors->property("opened").toBool(); }));
    EXPECT_EQ(errors->property("message").toString(), "длительность должна быть от 3 до 60 секунд");
    EXPECT_EQ(controller->durationSeconds(), 10);
    screenshot(*window, QStringLiteral("settings-error"));
}

} // namespace
} // namespace puls::gui

// Qt keeps process-wide scene graph, pixmap and font caches until exit;
// LeakSanitizer reports them when the tests run with sanitizers.
extern "C" const char* __lsan_default_suppressions();
extern "C" const char* __lsan_default_suppressions() {
    return "leak:libQt6\n";
}

int main(int argc, char** argv) {
    // Tests run without a display; the software renderer needs no GPU.
    const auto set_default = [](const char* name, const char* value) {
        if (qEnvironmentVariableIsEmpty(name)) {
            qputenv(name, value);
        }
    };
    set_default("QT_QPA_PLATFORM", "offscreen");
    set_default("QT_QUICK_BACKEND", "software");
    QGuiApplication application(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Material"));
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
