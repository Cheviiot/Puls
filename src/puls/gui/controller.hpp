#pragma once

#include "puls/application/runner.hpp"
#include "puls/core/context.hpp"
#include "puls/gui/model.hpp"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include <functional>
#include <memory>
#include <thread>

namespace puls::gui {

// Exposes the dashboard model to QML. Measurements run on a worker thread;
// runner events are queued to the interface thread before they reach the
// model, so QML only ever observes the interface thread's state.
class DashboardController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(QStringList serviceLabels READ serviceLabels CONSTANT)
    Q_PROPERTY(QStringList serviceNames READ serviceNames CONSTANT)
    Q_PROPERTY(QStringList profileNames READ profileNames CONSTANT)
    Q_PROPERTY(QStringList profileDetails READ profileDetails CONSTANT)
    Q_PROPERTY(QStringList connectionLabels READ connectionLabels CONSTANT)
    Q_PROPERTY(QStringList phaseLabels READ phaseLabels CONSTANT)
    Q_PROPERTY(QStringList phaseNames READ phaseNames CONSTANT)
    Q_PROPERTY(QStringList themeLabels READ themeLabels CONSTANT)
    Q_PROPERTY(int windowWidth READ windowWidth CONSTANT)
    Q_PROPERTY(int windowHeight READ windowHeight CONSTANT)
    Q_PROPERTY(int minimumWindowWidth READ minimumWindowWidth CONSTANT)
    Q_PROPERTY(int minimumWindowHeight READ minimumWindowHeight CONSTANT)
    Q_PROPERTY(bool systemDark READ systemDark NOTIFY systemDarkChanged)

    Q_PROPERTY(int serviceIndex READ serviceIndex NOTIFY settingsChanged)
    Q_PROPERTY(int profileIndex READ profileIndex NOTIFY settingsChanged)
    Q_PROPERTY(int durationSeconds READ durationSeconds NOTIFY settingsChanged)
    Q_PROPERTY(int connectionsIndex READ connectionsIndex NOTIFY settingsChanged)
    Q_PROPERTY(int phaseIndex READ phaseIndex NOTIFY settingsChanged)
    Q_PROPERTY(QString speedtestServer READ speedtestServer NOTIFY settingsChanged)
    Q_PROPERTY(bool showIp READ showIp NOTIFY settingsChanged)
    Q_PROPERTY(int themeIndex READ themeIndex NOTIFY settingsChanged)
    Q_PROPERTY(QString themeLabel READ themeLabel NOTIFY settingsChanged)
    Q_PROPERTY(QString settingsSummary READ settingsSummary NOTIFY settingsChanged)
    Q_PROPERTY(QString idleHint READ idleHint NOTIFY settingsChanged)

    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool measuring READ measuring NOTIFY stateChanged)
    Q_PROPERTY(bool detecting READ detecting NOTIFY stateChanged)
    Q_PROPERTY(bool stopping READ stopping NOTIFY stateChanged)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY stateChanged)
    Q_PROPERTY(QString startLabel READ startLabel NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(int statusTone READ statusTone NOTIFY stateChanged)
    Q_PROPERTY(QString heroValue READ heroValue NOTIFY stateChanged)
    Q_PROPERTY(QString heroUnit READ heroUnit NOTIFY stateChanged)
    Q_PROPERTY(QString heroLabel READ heroLabel NOTIFY stateChanged)
    Q_PROPERTY(QVariantList phases READ phases NOTIFY stateChanged)
    Q_PROPERTY(QString activeMetric READ activeMetric NOTIFY stateChanged)
    Q_PROPERTY(QString serviceProgress READ serviceProgress NOTIFY stateChanged)
    Q_PROPERTY(QString ping READ ping NOTIFY stateChanged)
    Q_PROPERTY(QString jitter READ jitter NOTIFY stateChanged)
    Q_PROPERTY(QString download READ download NOTIFY stateChanged)
    Q_PROPERTY(QString upload READ upload NOTIFY stateChanged)
    Q_PROPERTY(QString serverText READ serverText NOTIFY stateChanged)
    Q_PROPERTY(bool serverKnown READ serverKnown NOTIFY stateChanged)
    Q_PROPERTY(QString connectionText READ connectionText NOTIFY stateChanged)
    Q_PROPERTY(bool connectionKnown READ connectionKnown NOTIFY stateChanged)
    Q_PROPERTY(QString notices READ notices NOTIFY stateChanged)
    Q_PROPERTY(int noticeTone READ noticeTone NOTIFY stateChanged)
    Q_PROPERTY(QVariantList results READ results NOTIFY stateChanged)

public:
    // root is canceled when the application must stop, for example on Ctrl+C.
    DashboardController(const Context& root, QString version, app::RunnerOptions runner,
                        PreferenceStore& store, QObject* parent = nullptr);
    DashboardController(const DashboardController&) = delete;
    DashboardController& operator=(const DashboardController&) = delete;
    ~DashboardController() override;

    [[nodiscard]] QString version() const { return version_; }
    [[nodiscard]] QStringList serviceLabels() const;
    [[nodiscard]] QStringList serviceNames() const;
    [[nodiscard]] QStringList profileNames() const;
    [[nodiscard]] QStringList profileDetails() const;
    [[nodiscard]] QStringList connectionLabels() const;
    [[nodiscard]] QStringList phaseLabels() const;
    [[nodiscard]] QStringList phaseNames() const;
    [[nodiscard]] QStringList themeLabels() const;
    [[nodiscard]] int windowWidth() const;
    [[nodiscard]] int windowHeight() const;
    [[nodiscard]] int minimumWindowWidth() const;
    [[nodiscard]] int minimumWindowHeight() const;
    [[nodiscard]] bool systemDark() const { return system_dark_; }
    // Follows the platform color scheme for the "system" theme.
    void setSystemDark(bool dark);

    [[nodiscard]] ThemeMode theme() const { return model_.settings().theme; }
    [[nodiscard]] int serviceIndex() const;
    [[nodiscard]] int profileIndex() const;
    [[nodiscard]] int durationSeconds() const;
    [[nodiscard]] int connectionsIndex() const;
    [[nodiscard]] int phaseIndex() const;
    [[nodiscard]] QString speedtestServer() const;
    [[nodiscard]] bool showIp() const;
    [[nodiscard]] int themeIndex() const;
    [[nodiscard]] QString themeLabel() const;
    [[nodiscard]] QString settingsSummary() const;
    [[nodiscard]] QString idleHint() const;

    [[nodiscard]] bool busy() const { return model_.busy(); }
    [[nodiscard]] bool measuring() const { return model_.measuring(); }
    [[nodiscard]] bool detecting() const { return model_.detecting(); }
    [[nodiscard]] bool stopping() const { return model_.state().stopping; }
    [[nodiscard]] bool hasResult() const { return model_.state().has_result; }
    [[nodiscard]] QString startLabel() const;
    [[nodiscard]] QString status() const;
    [[nodiscard]] int statusTone() const;
    [[nodiscard]] QString heroValue() const;
    [[nodiscard]] QString heroUnit() const;
    [[nodiscard]] QString heroLabel() const;
    // [{title, state: pending|active|done|failed|skipped, progress}].
    [[nodiscard]] QVariantList phases() const;
    // "ping", "download", "upload" or empty.
    [[nodiscard]] QString activeMetric() const;
    [[nodiscard]] QString serviceProgress() const;
    [[nodiscard]] QString ping() const;
    [[nodiscard]] QString jitter() const;
    [[nodiscard]] QString download() const;
    [[nodiscard]] QString upload() const;
    [[nodiscard]] QString serverText() const;
    [[nodiscard]] bool serverKnown() const { return model_.state().server_known; }
    [[nodiscard]] QString connectionText() const;
    [[nodiscard]] bool connectionKnown() const { return model_.state().connection_known; }
    [[nodiscard]] QString notices() const;
    [[nodiscard]] int noticeTone() const;
    // [{service, status, tone, ping, download, upload, server}].
    [[nodiscard]] QVariantList results() const;

    // Starts a measurement or stops the active one.
    Q_INVOKABLE void toggleMeasurement();
    Q_INVOKABLE void detectConnection();
    Q_INVOKABLE void selectService(int index);
    Q_INVOKABLE void cycleTheme();
    // Returns an empty string on success and the validation error otherwise.
    Q_INVOKABLE QString applySettings(int service, int profile, const QString& duration,
                                      int connections, int phase, const QString& server,
                                      bool showIp, int theme);
    Q_INVOKABLE int profileDurationSeconds(int profile) const;
    Q_INVOKABLE void saveWindowSize(int width, int height);
    // Cancels the active operation and waits for its worker thread.
    Q_INVOKABLE void shutdown();
    // Stops the active measurement or connection lookup.
    Q_INVOKABLE void cancel();

signals:
    void systemDarkChanged();
    void settingsChanged();
    void stateChanged();
    void errorOccurred(const QString& message);

private:
    using Job = std::function<void(const Context&, const app::Observer&)>;

    void start(Job job);
    void join();
    template <class Function>
    void post(Function&& function);

    Context root_;
    QString version_;
    app::RunnerOptions runner_;
    Dashboard model_;
    std::unique_ptr<CancelScope> active_;
    std::thread worker_;
    bool system_dark_ = false;
};

// Keeps the parts of the window that the system draws in the theme of the
// dashboard: the window buttons on Windows, the title bar on macOS and the
// system bars on Android. The "system" theme follows the color scheme of the
// system.
void follow_system_theme(DashboardController& controller);

} // namespace puls::gui
