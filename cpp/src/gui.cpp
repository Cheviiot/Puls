#include "puls/gui.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyleFactory>
#include <QStyleHints>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <utility>

namespace puls {
namespace {
QString phaseName(const QString& phase) {
    if (phase == "select") return QStringLiteral("Выбор сервера измерения…");
    if (phase == "ping") return QStringLiteral("Измеряем задержку…");
    if (phase == "download") return QStringLiteral("Измеряем скорость загрузки…");
    if (phase == "upload") return QStringLiteral("Измеряем скорость отдачи…");
    return QStringLiteral("Выполняется измерение…");
}

QLabel* label(const QString& text, QWidget* parent = nullptr) {
    auto* result = new QLabel(text, parent);
    result->setWordWrap(true);
    result->setTextFormat(Qt::PlainText);
    result->setMinimumWidth(0);
    result->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    return result;
}

class Window final : public QMainWindow {
public:
    Window(const Config& initial, BackendFactory factory)
        : factory_(std::move(factory)) {
        setObjectName("pulsWindow");
        setWindowTitle(QStringLiteral("Puls"));
        setWindowIcon(QIcon(QStringLiteral(":/puls/Icon.png")));
        setMinimumSize(360, 294);
        load(initial);
        build();
        applyTheme();
        rebuildResults();
        connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            bool background = state == Qt::ApplicationHidden || state == Qt::ApplicationSuspended;
#ifdef Q_OS_ANDROID
            background = background || state == Qt::ApplicationInactive;
#endif
            if (background) cancel();
        });
        connect(qApp->styleHints(), &QStyleHints::colorSchemeChanged, this, [this] {
            if (theme_ == "system") applyTheme();
        });
    }

    ~Window() override {
        if (cancellation_) cancellation_->cancel();
        if (thread_) {
            thread_->wait();
            delete thread_;
        }
    }

    bool preview(const QString& directory) {
        if (!QDir().mkpath(directory)) return false;
        bool saved = true;
        config_.service = "all";
        const QList<QSize> sizes{{360, 294}, {360, 640}, {900, 600}, {1280, 800}, {360, 294}};
        for (const auto& theme : QStringList{"light", "dark"}) {
            theme_ = theme;
            applyTheme();
            for (const auto& state : QStringList{"initial", "results", "error", "running"}) {
                rebuildResults();
                status_->setText(QStringLiteral("Готово к проверке"));
                action_->setText(QStringLiteral("Начать проверку"));
                if (state == "results" || state == "error") {
                    QJsonArray results;
                    for (const auto& id : QStringList{"yandex", "speedtest"}) {
                        QJsonObject phases;
                        for (const auto& phase : QStringList{"ping", "download", "upload"}) {
                            auto result = emptyPhase("ok");
                            result.insert(phase == "ping" ? "value_ms" : "mbps", phase == "ping" ? 12.34 : phase == "download" ? 420.5 : 95.25);
                            if (state == "error") {
                                result = failedPhase(OpError("network", phase, QStringLiteral("Сервер измерения временно недоступен. Проверьте подключение к интернету и повторите проверку.")));
                            }
                            phases.insert(phase, result);
                        }
                        results.append(QJsonObject{{"service", id}, {"status", state == "error" ? "error" : "ok"},
                            {"server", QJsonObject{{"name", QStringLiteral("Синтетический сервер измерения")}, {"city", QStringLiteral("Владивосток")}}},
                            {"phases", phases}, {"warnings", QJsonArray{}}});
                    }
                    finish(QJsonObject{{"status", state == "error" ? "error" : "ok"}, {"results", results}});
                } else if (state == "running") {
                    status_->setText(QStringLiteral("Яндекс.Интернетометр · Измеряем скорость загрузки…"));
                    action_->setText(QStringLiteral("Остановить"));
                }
                for (qsizetype index = 0; index < sizes.size(); ++index) {
                    resize(sizes[index]);
                    QApplication::processEvents();
                    centralWidget()->layout()->activate();
                    QApplication::processEvents();
                    const auto filename = QStringLiteral("%1/%2-%3-%4x%5%6.png")
                        .arg(directory, theme, state).arg(width()).arg(height())
                        .arg(index == sizes.size() - 1 ? "-return" : "");
                    saved = grab().save(filename) && saved;
                }
            }
            pages_->setCurrentWidget(settings_);
            resize(360, 640);
            QApplication::processEvents();
            saved = grab().save(directory + '/' + theme + "-settings-360x640.png") && saved;
            pages_->setCurrentWidget(measure_);
        }
        return saved;
    }

protected:
    void closeEvent(QCloseEvent* event) override {
        if (thread_) {
            closePending_ = true;
            cancel();
            event->ignore();
            return;
        }
        saveSize();
        QMainWindow::closeEvent(event);
    }

    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        if (sizeTimer_) sizeTimer_->start();
    }

private:
    struct Rows {
        QFrame* box = nullptr;
        QLabel* server = nullptr;
        QLabel* ping = nullptr;
        QLabel* download = nullptr;
        QLabel* upload = nullptr;
        QLabel* error = nullptr;
    };

    Config config_;
    BackendFactory factory_;
    QString theme_ = "system";
    QStackedWidget* pages_ = nullptr;
    QWidget* measure_ = nullptr;
    QWidget* settings_ = nullptr;
    QVBoxLayout* resultsLayout_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* summary_ = nullptr;
    QLabel* connection_ = nullptr;
    QPushButton* action_ = nullptr;
    QPushButton* menu_ = nullptr;
    QComboBox* service_ = nullptr;
    QComboBox* profile_ = nullptr;
    QComboBox* phase_ = nullptr;
    QComboBox* themes_ = nullptr;
    QSpinBox* duration_ = nullptr;
    QSpinBox* connections_ = nullptr;
    QTimer* sizeTimer_ = nullptr;
    QThread* thread_ = nullptr;
    std::shared_ptr<Cancellation> cancellation_;
    QHash<QString, Rows> rows_;
    bool closePending_ = false;

    void load(const Config& initial) {
        QSettings preferences("Puls", "Puls");
        config_ = initial;
        if (config_.service.isEmpty()) {
            config_.service = preferences.value("service", "yandex").toString();
            config_.profile = preferences.value("profile", "balanced").toString();
            config_.durationSeconds = preferences.value("duration", 10).toInt();
            config_.connections = preferences.value("connections", 0).toInt();
            config_.only = preferences.value("phase", "all").toString();
        }
        if (config_.service != "yandex" && config_.service != "speedtest" && config_.service != "all") config_.service = "yandex";
        if (config_.profile != "quick" && config_.profile != "balanced" && config_.profile != "accurate") config_.profile = "balanced";
        if (config_.only != "all" && config_.only != "ping" && config_.only != "download" && config_.only != "upload") config_.only = "all";
        config_.durationSeconds = std::clamp(config_.durationSeconds, 3, 60);
        config_.connections = std::clamp(config_.connections, 0, 16);
        config_.command = Command::Measure;
        config_.server.clear();
        config_.json = false;
        theme_ = preferences.value("theme", "system").toString();
        if (theme_ != "system" && theme_ != "light" && theme_ != "dark") theme_ = "system";
        const auto size = preferences.value("windowSize", QSize(500, 640)).toSize();
        resize(std::clamp(size.width(), 360, 4096), std::clamp(size.height(), 294, 2160));
    }

    void saveOptions() {
        if (!qEnvironmentVariableIsEmpty("PULS_GUI_PREVIEW_DIR")) return;
        QSettings preferences("Puls", "Puls");
        preferences.setValue("theme", theme_);
        preferences.setValue("service", config_.service);
        preferences.setValue("profile", config_.profile);
        preferences.setValue("duration", config_.durationSeconds);
        preferences.setValue("connections", config_.connections);
        preferences.setValue("phase", config_.only);
    }

    void saveSize() {
        if (!qEnvironmentVariableIsEmpty("PULS_GUI_PREVIEW_DIR")) return;
        if (!isMaximized() && !isFullScreen()) {
            QSettings preferences("Puls", "Puls");
            preferences.setValue("windowSize", size());
        }
    }

    QScrollArea* scroll(QWidget* content) {
        auto* area = new QScrollArea;
        area->setWidgetResizable(true);
        area->setFrameShape(QFrame::NoFrame);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        area->setWidget(content);
        return area;
    }

    QWidget* centered(QWidget* content) {
        auto* outer = new QWidget;
        auto* layout = new QHBoxLayout(outer);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addStretch();
        content->setMaximumWidth(720);
        content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        layout->addWidget(content, 1);
        layout->addStretch();
        return outer;
    }

    void build() {
        pages_ = new QStackedWidget;
        pages_->setObjectName("mainPages");
        setCentralWidget(pages_);
        measure_ = new QWidget;
        measure_->setObjectName("measurePage");
        auto* mainLayout = new QVBoxLayout(measure_);
        mainLayout->setContentsMargins(16, 8, 16, 16);
        mainLayout->setSpacing(8);
        auto* toolbar = new QHBoxLayout;
        toolbar->addStretch();
        menu_ = new QPushButton(QStringLiteral("☰"));
        menu_->setObjectName("menuButton");
        menu_->setAccessibleName(QStringLiteral("Меню"));
        menu_->setToolTip(QStringLiteral("Меню"));
        menu_->setFixedSize(40, 36);
        menu_->setProperty("flatControl", true);
        toolbar->addWidget(menu_);
        mainLayout->addLayout(toolbar);

        auto* content = new QWidget;
        content->setObjectName("resultContent");
        auto* contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(0, 4, 0, 8);
        contentLayout->setSpacing(12);
        summary_ = label("");
        summary_->setObjectName("settingsSummary");
        summary_->setProperty("secondary", true);
        contentLayout->addWidget(summary_);
        resultsLayout_ = new QVBoxLayout;
        resultsLayout_->setSpacing(12);
        contentLayout->addLayout(resultsLayout_);
        connection_ = label("");
        connection_->setObjectName("connectionLabel");
        connection_->setProperty("secondary", true);
        connection_->hide();
        contentLayout->addWidget(connection_);
        contentLayout->addStretch();
        auto* resultScroll = scroll(centered(content));
        resultScroll->setObjectName("resultsScroll");
        mainLayout->addWidget(resultScroll, 1);

        auto* footer = new QWidget;
        auto* footerLayout = new QVBoxLayout(footer);
        footerLayout->setContentsMargins(0, 4, 0, 0);
        footerLayout->setSpacing(8);
        status_ = label(QStringLiteral("Готово к проверке"));
        status_->setObjectName("statusLabel");
        status_->setAlignment(Qt::AlignCenter);
        status_->setProperty("secondary", true);
        footerLayout->addWidget(status_);
        action_ = new QPushButton(QStringLiteral("Начать проверку"));
        action_->setObjectName("startButton");
        action_->setAccessibleName(QStringLiteral("Начать проверку"));
        action_->setProperty("primaryAction", true);
        action_->setMinimumHeight(44);
        action_->setMaximumWidth(360);
        footerLayout->addWidget(action_, 0, Qt::AlignHCenter);
        mainLayout->addWidget(centered(footer));
        pages_->addWidget(measure_);
        buildSettings();
        connect(action_, &QPushButton::clicked, this, [this] {
            if (thread_) cancel(); else start();
        });
        connect(menu_, &QPushButton::clicked, this, [this] {
            QMenu menu(this);
            auto* settingsAction = menu.addAction(QStringLiteral("Параметры"));
            settingsAction->setEnabled(thread_ == nullptr);
            auto* aboutAction = menu.addAction(QStringLiteral("О приложении"));
            const auto selected = menu.exec(menu_->mapToGlobal(QPoint(menu_->width(), menu_->height())));
            if (selected == settingsAction) pages_->setCurrentWidget(settings_);
            else if (selected == aboutAction) QMessageBox::about(this, QStringLiteral("Puls"),
                versionText().trimmed() + QStringLiteral("\n\nПроверка скорости и качества интернета\nЯндекс.Интернетометр и speedtest.ru"));
        });
        sizeTimer_ = new QTimer(this);
        sizeTimer_->setSingleShot(true);
        sizeTimer_->setInterval(300);
        connect(sizeTimer_, &QTimer::timeout, this, [this] { saveSize(); });
    }

    void buildSettings() {
        settings_ = new QWidget;
        settings_->setObjectName("settingsPage");
        auto* layout = new QVBoxLayout(settings_);
        layout->setContentsMargins(16, 8, 16, 16);
        auto* header = new QHBoxLayout;
        auto* back = new QPushButton(QStringLiteral("‹ Назад"));
        back->setObjectName("backButton");
        back->setProperty("flatControl", true);
        header->addWidget(back);
        auto* heading = label(QStringLiteral("Параметры"));
        heading->setAlignment(Qt::AlignCenter);
        heading->setProperty("heading", true);
        header->addWidget(heading, 1);
        header->addSpacing(back->sizeHint().width());
        layout->addLayout(header);
        auto* content = new QWidget;
        auto* form = new QFormLayout(content);
        form->setContentsMargins(0, 12, 0, 12);
        form->setSpacing(14);
        form->setRowWrapPolicy(QFormLayout::WrapAllRows);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        service_ = new QComboBox;
        service_->setObjectName("serviceChoice");
        service_->addItem(QStringLiteral("Яндекс.Интернетометр"), "yandex");
        service_->addItem(QStringLiteral("speedtest.ru"), "speedtest");
        service_->addItem(QStringLiteral("Оба сервиса последовательно"), "all");
        service_->setCurrentIndex(service_->findData(config_.service));
        form->addRow(QStringLiteral("Сервис измерения"), service_);
        profile_ = new QComboBox;
        profile_->setObjectName("profileChoice");
        profile_->addItem(QStringLiteral("Быстрый · 5 секунд"), "quick");
        profile_->addItem(QStringLiteral("Сбалансированный · 10 секунд"), "balanced");
        profile_->addItem(QStringLiteral("Точный · 15 секунд"), "accurate");
        profile_->setCurrentIndex(profile_->findData(config_.profile));
        form->addRow(QStringLiteral("Профиль"), profile_);
        duration_ = new QSpinBox;
        duration_->setObjectName("durationSpin");
        duration_->setRange(3, 60);
        duration_->setSuffix(QStringLiteral(" с"));
        duration_->setValue(config_.durationSeconds);
        form->addRow(QStringLiteral("Длительность каждой фазы"), duration_);
        connections_ = new QSpinBox;
        connections_->setObjectName("connectionsSpin");
        connections_->setRange(0, 16);
        connections_->setSpecialValueText(QStringLiteral("Автоматически"));
        connections_->setValue(config_.connections);
        form->addRow(QStringLiteral("Соединения"), connections_);
        phase_ = new QComboBox;
        phase_->setObjectName("phaseChoice");
        phase_->addItem(QStringLiteral("Все этапы"), "all");
        phase_->addItem(QStringLiteral("Только задержка"), "ping");
        phase_->addItem(QStringLiteral("Только загрузка"), "download");
        phase_->addItem(QStringLiteral("Только отдача"), "upload");
        phase_->setCurrentIndex(phase_->findData(config_.only));
        form->addRow(QStringLiteral("Этапы проверки"), phase_);
        themes_ = new QComboBox;
        themes_->setObjectName("themeChoice");
        themes_->addItem(QStringLiteral("Системная"), "system");
        themes_->addItem(QStringLiteral("Светлая"), "light");
        themes_->addItem(QStringLiteral("Тёмная"), "dark");
        themes_->setCurrentIndex(themes_->findData(theme_));
        form->addRow(QStringLiteral("Оформление"), themes_);
        layout->addWidget(scroll(centered(content)), 1);
        pages_->addWidget(settings_);
        connect(back, &QPushButton::clicked, this, [this] {
            rebuildResults();
            pages_->setCurrentWidget(measure_);
        });
        connect(service_, &QComboBox::currentIndexChanged, this, [this] {
            config_.service = service_->currentData().toString(); saveOptions();
        });
        connect(profile_, &QComboBox::currentIndexChanged, this, [this] {
            config_.profile = profile_->currentData().toString();
            duration_->setValue(config_.profile == "quick" ? 5 : config_.profile == "accurate" ? 15 : 10);
            saveOptions();
        });
        connect(duration_, &QSpinBox::valueChanged, this, [this](int value) {
            config_.durationSeconds = value; saveOptions();
        });
        connect(connections_, &QSpinBox::valueChanged, this, [this](int value) {
            config_.connections = value; saveOptions();
        });
        connect(phase_, &QComboBox::currentIndexChanged, this, [this] {
            config_.only = phase_->currentData().toString(); saveOptions();
        });
        connect(themes_, &QComboBox::currentIndexChanged, this, [this] {
            theme_ = themes_->currentData().toString(); applyTheme(); saveOptions();
        });
    }

    QLabel* row(QVBoxLayout* layout, const QString& title, const QString& name) {
        auto* widget = new QWidget;
        auto* line = new QHBoxLayout(widget);
        line->setContentsMargins(14, 12, 14, 12);
        line->setSpacing(12);
        auto* caption = label(title);
        auto* value = label(QStringLiteral("—"));
        value->setObjectName(name);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        value->setProperty("metricValue", true);
        line->addWidget(caption, 1);
        line->addWidget(value, 1);
        layout->addWidget(widget);
        return value;
    }

    void separator(QVBoxLayout* layout) {
        auto* line = new QFrame;
        line->setObjectName("rowSeparator");
        line->setFixedHeight(1);
        layout->addWidget(line);
    }

    void rebuildResults() {
        while (auto* item = resultsLayout_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        rows_.clear();
        const auto services = config_.service == "all" ? QStringList{"yandex", "speedtest"} : QStringList{config_.service};
        for (const auto& id : services) {
            auto* card = new QWidget;
            card->setObjectName("serviceCard_" + id);
            auto* outer = new QVBoxLayout(card);
            outer->setContentsMargins(0, 0, 0, 0);
            outer->setSpacing(8);
            auto* title = label(displayName(id));
            title->setProperty("heading", true);
            outer->addWidget(title);
            Rows values;
            values.box = new QFrame;
            values.box->setObjectName("resultBox");
            auto* content = new QVBoxLayout(values.box);
            content->setContentsMargins(0, 0, 0, 0);
            content->setSpacing(0);
            values.ping = row(content, QStringLiteral("Задержка"), "result_" + id + "_ping");
            separator(content);
            values.download = row(content, QStringLiteral("Загрузка"), "result_" + id + "_download");
            separator(content);
            values.upload = row(content, QStringLiteral("Отдача"), "result_" + id + "_upload");
            outer->addWidget(values.box);
            values.server = label(QStringLiteral("Сервер измерения будет выбран автоматически"));
            values.server->setObjectName("server_" + id);
            values.server->setProperty("secondary", true);
            outer->addWidget(values.server);
            values.error = label("");
            values.error->setObjectName("error_" + id);
            values.error->setProperty("errorText", true);
            values.error->hide();
            outer->addWidget(values.error);
            resultsLayout_->addWidget(card);
            rows_.insert(id, values);
        }
        summary_->setText(QStringLiteral("%1 с на этап · %2")
                              .arg(config_.durationSeconds)
                              .arg(config_.connections == 0 ? QStringLiteral("соединения автоматически")
                                                           : QStringLiteral("соединений: %1").arg(config_.connections)));
    }

    void applyTheme() {
        const bool dark = theme_ == "dark" ||
            (theme_ == "system" && qApp->styleHints()->colorScheme() == Qt::ColorScheme::Dark);
        const QColor background(dark ? "#222226" : "#fafafb");
        const QColor surface(dark ? "#303034" : "#ffffff");
        const QColor foreground(dark ? "#f4f4f5" : "#202024");
        const QColor secondary(dark ? "#b4b4be" : "#686872");
        const QColor border(dark ? "#45454d" : "#dedee4");
        const QColor accent(dark ? "#78aeed" : "#3584e4");
        auto palette = qApp->palette();
        palette.setColor(QPalette::Window, background);
        palette.setColor(QPalette::WindowText, foreground);
        palette.setColor(QPalette::Base, surface);
        palette.setColor(QPalette::AlternateBase, background);
        palette.setColor(QPalette::Text, foreground);
        palette.setColor(QPalette::Button, surface);
        palette.setColor(QPalette::ButtonText, foreground);
        palette.setColor(QPalette::Highlight, accent);
        palette.setColor(QPalette::HighlightedText, QColor("#ffffff"));
        qApp->setPalette(palette);
        setStyleSheet(QStringLiteral(R"(
            QMainWindow, QStackedWidget, QScrollArea, QScrollArea > QWidget > QWidget { background: %1; }
            QWidget { color: %2; }
            QLabel { background: transparent; }
            QLabel[secondary="true"] { color: %3; }
            QLabel[heading="true"], QLabel[metricValue="true"] { font-weight: 600; }
            QLabel[errorText="true"] { color: %4; }
            QFrame#resultBox { background: %5; border: 1px solid %6; border-radius: 12px; }
            QFrame#rowSeparator { background: %6; margin-left: 14px; margin-right: 14px; }
            QPushButton { background: %5; border: 1px solid %6; border-radius: 8px; padding: 8px 14px; }
            QPushButton:hover { border-color: %7; }
            QPushButton:focus, QComboBox:focus, QSpinBox:focus { border: 2px solid %7; }
            QPushButton[flatControl="true"] { background: transparent; border: 0; }
            QPushButton[primaryAction="true"] { background: #3584e4; color: white; border: 0; border-radius: 22px; padding: 10px 28px; font-weight: 600; }
            QPushButton[primaryAction="true"]:hover { background: #2674d3; }
            QPushButton[primaryAction="true"]:focus { border: 2px solid %2; }
            QPushButton:disabled { color: %3; }
            QComboBox, QSpinBox { background: %5; border: 1px solid %6; border-radius: 8px; padding: 8px; min-height: 22px; }
            QComboBox QAbstractItemView { background: %5; color: %2; selection-background-color: #3584e4; }
            QMenu { background: %5; border: 1px solid %6; border-radius: 8px; padding: 6px; }
            QMenu::item { padding: 8px 16px; }
            QMenu::item:selected { background: #3584e4; color: white; border-radius: 5px; }
        )").arg(background.name(), foreground.name(), secondary.name(),
                 dark ? QStringLiteral("#ff938d") : QStringLiteral("#c01c28"), surface.name(), border.name(), accent.name()));
    }

    void start() {
        rebuildResults();
        connection_->clear();
        connection_->hide();
        status_->setText(QStringLiteral("Начинаем проверку…"));
        action_->setText(QStringLiteral("Остановить"));
        action_->setAccessibleName(QStringLiteral("Остановить проверку"));
        action_->setEnabled(true);
        cancellation_ = std::make_shared<Cancellation>();
        auto config = config_;
        const auto cancellation = cancellation_;
        const auto factory = factory_;
        thread_ = QThread::create([this, config, cancellation, factory] {
            try {
                Runner runner(factory);
                const auto result = runner.run(config, *cancellation, [this](const QJsonObject& event) {
                    QMetaObject::invokeMethod(this, [this, event] { observe(event); }, Qt::QueuedConnection);
                });
                QMetaObject::invokeMethod(this, [this, result] { finish(result); }, Qt::QueuedConnection);
            } catch (const std::exception& error) {
                const auto message = QString::fromUtf8(error.what());
                QMetaObject::invokeMethod(this, [this, message] {
                    status_->setText(QStringLiteral("Не удалось выполнить проверку: %1").arg(message));
                }, Qt::QueuedConnection);
            }
        });
        connect(thread_, &QThread::finished, this, [this] {
            thread_->deleteLater();
            thread_ = nullptr;
            cancellation_.reset();
            action_->setText(QStringLiteral("Начать проверку"));
            action_->setAccessibleName(QStringLiteral("Начать проверку"));
            action_->setEnabled(true);
            if (closePending_) close();
        });
        thread_->start();
    }

    void cancel() {
        if (!cancellation_) return;
        cancellation_->cancel();
        status_->setText(QStringLiteral("Останавливаем проверку…"));
        action_->setEnabled(false);
    }

    void phaseResult(const QString& id, const QString& phase, const QJsonObject& result) {
        if (!rows_.contains(id)) return;
        const auto& rows = rows_[id];
        QLabel* value = phase == "ping" ? rows.ping : phase == "download" ? rows.download : phase == "upload" ? rows.upload : nullptr;
        if (!value) return;
        const auto status = result.value("status").toString();
        const auto number = result.value(phase == "ping" ? "value_ms" : "mbps");
        if (number.isDouble()) value->setText(QStringLiteral("%1 %2").arg(number.toDouble(), 0, 'f', 2)
                                                .arg(phase == "ping" ? QStringLiteral("мс") : QStringLiteral("Мбит/с")));
        else if (status == "error") value->setText(QStringLiteral("Ошибка"));
        else if (status == "canceled") value->setText(QStringLiteral("Остановлено"));
        else value->setText(QStringLiteral("—"));
        const auto message = result.value("error").toObject().value("message").toString();
        if (!message.isEmpty()) {
            rows.error->setText(message);
            rows.error->show();
        }
    }

    void serverResult(const QString& id, const QJsonObject& server) {
        if (!rows_.contains(id) || server.isEmpty()) return;
        QString text = QStringLiteral("Сервер измерения: ") + server.value("name").toString();
        if (!server.value("city").toString().isEmpty()) text += QStringLiteral(" · ") + server.value("city").toString();
        if (!server.value("region").toString().isEmpty()) text += QStringLiteral(", ") + server.value("region").toString();
        rows_[id].server->setText(text);
    }

    void observe(const QJsonObject& event) {
        const auto kind = event.value("kind").toString();
        const auto id = event.value("service").toString();
        const auto phase = event.value("phase").toString();
        if (kind == "phase_started") {
            status_->setText(displayName(id) + QStringLiteral(" · ") + phaseName(phase));
        } else if (kind == "server_selected") serverResult(id, event.value("server").toObject());
        else if (kind == "phase_completed") phaseResult(id, phase, event.value("phase_result").toObject());
    }

    void finish(const QJsonObject& envelope) {
        for (const auto value : envelope.value("results").toArray()) {
            const auto result = value.toObject();
            const auto id = result.value("service").toString();
            serverResult(id, result.value("server").toObject());
            const auto phases = result.value("phases").toObject();
            for (const auto& phase : QStringList{"ping", "download", "upload"})
                phaseResult(id, phase, phases.value(phase).toObject());
            if (rows_.contains(id)) {
                QStringList messages;
                const auto error = result.value("error").toObject().value("message").toString();
                if (!error.isEmpty()) messages.append(error);
                for (const auto warning : result.value("warnings").toArray()) messages.append(warning.toString());
                if (!messages.isEmpty()) {
                    rows_[id].error->setText(messages.join('\n'));
                    rows_[id].error->show();
                }
            }
        }
        const auto connection = envelope.value("connection").toObject();
        if (connection.value("status") == "ok") {
            QStringList parts;
            if (!connection.value("external_ip").toString().isEmpty()) parts.append(connection.value("external_ip").toString());
            if (!connection.value("isp").toString().isEmpty()) parts.append(connection.value("isp").toString());
            connection_->setText(parts.join(QStringLiteral(" · ")));
            connection_->setVisible(!parts.isEmpty());
        }
        const auto status = envelope.value("status").toString();
        status_->setText(status == "ok" ? QStringLiteral("Проверка завершена") :
                         status == "partial" ? QStringLiteral("Получен частичный результат") :
                         status == "canceled" ? QStringLiteral("Проверка остановлена") :
                                                QStringLiteral("Не удалось выполнить проверку"));
    }
};
} // namespace

QWidget* createMainWindow(const Config& initial, BackendFactory factory) {
    return new Window(initial, std::move(factory));
}

int runGUI(int& argc, char** argv, const Config& initial) {
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Puls"));
    QApplication::setOrganizationName(QStringLiteral("Puls"));
    QApplication::setApplicationVersion(versionText().trimmed().section(' ', 1));
    QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    auto font = application.font();
    if (QFontDatabase::families().contains(QStringLiteral("Adwaita Sans"))) font.setFamily(QStringLiteral("Adwaita Sans"));
    font.setPointSize(11);
    application.setFont(font);
    std::unique_ptr<QWidget> window(createMainWindow(initial));
    window->show();
    const auto previewDirectory = qEnvironmentVariable("PULS_GUI_PREVIEW_DIR");
    if (!previewDirectory.isEmpty()) {
        QTimer::singleShot(0, window.get(), [&application, &window, previewDirectory] {
            application.exit(static_cast<Window*>(window.get())->preview(previewDirectory) ? 0 : 1);
        });
    }
    return application.exec();
}
} // namespace puls
