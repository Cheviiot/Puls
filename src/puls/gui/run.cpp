#include "puls/gui/run.hpp"

#include "puls/gui/controller.hpp"
#include "puls/gui/settings_store.hpp"

#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QStyleHints>
#include <QUrl>
#include <QVariant>

#include <cstdlib>

namespace puls::gui {

namespace {

// Without X11 or Wayland Qt aborts the process, so report it as an error.
Error check_display() {
#if defined(__linux__) && !defined(__ANDROID__)
    const auto defined = [](const char* name) {
        const char* value = std::getenv(name);
        return value != nullptr && *value != '\0';
    };
    if (!defined("DISPLAY") && !defined("WAYLAND_DISPLAY") && !defined("QT_QPA_PLATFORM")) {
        return Error::make("нет графического окружения: не заданы DISPLAY и WAYLAND_DISPLAY");
    }
#endif
    return {};
}

bool system_dark() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
    return QGuiApplication::palette().color(QPalette::Window).lightness() < 128;
#endif
}

} // namespace

Error run(const Context& ctx, const Options& options) {
    if (Error error = check_display()) {
        return error;
    }
    if (ctx.done()) {
        return {};
    }
    // Qt keeps references to the arguments for the lifetime of the application.
    static int argc = 1;
    static char name[] = "puls";
    static char* argv[] = {name, nullptr};
    QGuiApplication::setOrganizationName(QStringLiteral("Cheviiot"));
    QGuiApplication::setOrganizationDomain(QStringLiteral("cheviiot.github.io"));
    QGuiApplication::setApplicationName(QStringLiteral("Puls"));
    QGuiApplication::setApplicationVersion(QString::fromStdString(options.version));
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.cheviiot.puls"));
    QGuiApplication application(argc, argv);
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/puls/assets/Icon.png")));
    QQuickStyle::setStyle(QStringLiteral("Material"));

    SettingsStore store;
    app::RunnerOptions runner;
    runner.log = options.log;
    DashboardController controller(ctx, QString::fromStdString(options.version), std::move(runner),
                                   store);
    controller.setSystemDark(system_dark());
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &controller,
                     [&controller](Qt::ColorScheme scheme) {
                         controller.setSystemDark(scheme == Qt::ColorScheme::Dark);
                     });
#endif
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    // A measurement does not continue in the background.
    QObject::connect(&application, &QGuiApplication::applicationStateChanged, &controller,
                     [&controller](Qt::ApplicationState state) {
                         if (state != Qt::ApplicationActive) {
                             controller.cancel();
                         }
                     });
#endif

    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("dashboard"), QVariant::fromValue(&controller)}});
    engine.load(QUrl(QStringLiteral("qrc:/puls/qml/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return Error::make("не удалось загрузить графический интерфейс");
    }
    const ContextCallback stop = ctx.on_done([&application] {
        QMetaObject::invokeMethod(
            &application, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
    });
    QGuiApplication::exec();
    controller.shutdown();
    return {};
}

} // namespace puls::gui
