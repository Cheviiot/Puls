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
    Q_PROPERTY(QStringList profileLabels READ profileLabels CONSTANT)
    Q_PROPERTY(QStringList connectionLabels READ connectionLabels CONSTANT)
    Q_PROPERTY(QStringList phaseLabels READ phaseLabels CONSTANT)
    Q_PROPERTY(QStringList themeLabels READ themeLabels CONSTANT)
    Q_PROPERTY(int windowWidth READ windowWidth CONSTANT)
    Q_PROPERTY(int windowHeight READ windowHeight CONSTANT)
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

    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool stopping READ stopping NOTIFY stateChanged)
    Q_PROPERTY(QString startLabel READ startLabel NOTIFY stateChanged)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    Q_PROPERTY(int statusTone READ statusTone NOTIFY stateChanged)
    Q_PROPERTY(QString currentValue READ currentValue NOTIFY stateChanged)
    Q_PROPERTY(QString currentUnit READ currentUnit NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY stateChanged)
    Q_PROPERTY(bool progressVisible READ progressVisible NOTIFY stateChanged)
    Q_PROPERTY(QString ping READ ping NOTIFY stateChanged)
    Q_PROPERTY(QString jitter READ jitter NOTIFY stateChanged)
    Q_PROPERTY(QString download READ download NOTIFY stateChanged)
    Q_PROPERTY(QString upload READ upload NOTIFY stateChanged)
    Q_PROPERTY(QString serverText READ serverText NOTIFY stateChanged)
    Q_PROPERTY(QString connectionText READ connectionText NOTIFY stateChanged)
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
    [[nodiscard]] QStringList profileLabels() const;
    [[nodiscard]] QStringList connectionLabels() const;
    [[nodiscard]] QStringList phaseLabels() const;
    [[nodiscard]] QStringList themeLabels() const;
    [[nodiscard]] int windowWidth() const;
    [[nodiscard]] int windowHeight() const;
    [[nodiscard]] bool systemDark() const { return system_dark_; }
    // Follows the platform color scheme for the "system" theme.
    void setSystemDark(bool dark);

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

    [[nodiscard]] bool busy() const { return model_.busy(); }
    [[nodiscard]] bool stopping() const;
    [[nodiscard]] QString startLabel() const;
    [[nodiscard]] QString status() const;
    [[nodiscard]] int statusTone() const;
    [[nodiscard]] QString currentValue() const;
    [[nodiscard]] QString currentUnit() const;
    [[nodiscard]] double progress() const;
    [[nodiscard]] bool progressVisible() const;
    [[nodiscard]] QString ping() const;
    [[nodiscard]] QString jitter() const;
    [[nodiscard]] QString download() const;
    [[nodiscard]] QString upload() const;
    [[nodiscard]] QString serverText() const;
    [[nodiscard]] QString connectionText() const;
    [[nodiscard]] QString notices() const;
    [[nodiscard]] int noticeTone() const;
    [[nodiscard]] QVariantList results() const;

    // Starts a measurement, or stops the active measurement or lookup.
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

} // namespace puls::gui
