#include "puls/core.hpp"
#ifdef PULS_BUILD_GUI
#include "puls/gui.hpp"
#endif

#include <QCoreApplication>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QSslSocket>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {
static_assert(std::atomic_bool::is_always_lock_free, "Signal handling requires lock-free atomic flags");
std::atomic_bool interrupted{false};
void handleSignal(int) { interrupted.store(true, std::memory_order_relaxed); }

class SignalMonitor {
public:
    explicit SignalMonitor(puls::Cancellation& cancellation) {
        interrupted.store(false);
        oldInterrupt_ = std::signal(SIGINT, handleSignal);
        oldTerminate_ = std::signal(SIGTERM, handleSignal);
        worker_ = std::thread([this, &cancellation] {
            while (!stopping_.load()) {
                if (interrupted.load()) cancellation.cancel();
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        });
    }
    ~SignalMonitor() {
        stopping_.store(true);
        worker_.join();
        std::signal(SIGINT, oldInterrupt_);
        std::signal(SIGTERM, oldTerminate_);
    }
private:
    using Handler = void (*)(int);
    Handler oldInterrupt_ = SIG_DFL;
    Handler oldTerminate_ = SIG_DFL;
    std::atomic_bool stopping_{false};
    std::thread worker_;
};

bool writeOutput(FILE* destination, const QByteArray& data) {
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(destination)));
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode)) {
        const auto text = QString::fromUtf8(data);
        qsizetype offset = 0;
        while (offset < text.size()) {
            const auto count = static_cast<DWORD>(qMin<qsizetype>(text.size() - offset, 16384));
            DWORD written = 0;
            if (!WriteConsoleW(handle, text.utf16() + offset, count, &written, nullptr) || written == 0) return false;
            offset += written;
        }
        return true;
    }
#endif
    qsizetype offset = 0;
    while (offset < data.size()) {
        const auto written = std::fwrite(data.constData() + offset, 1,
                                        static_cast<size_t>(data.size() - offset), destination);
        if (written == 0) return false;
        offset += static_cast<qsizetype>(written);
    }
    return std::fflush(destination) == 0 && std::ferror(destination) == 0;
}

void errorOutput(const QString& text) { writeOutput(stderr, text.toUtf8()); }

bool terminal(FILE* stream) {
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
    DWORD mode = 0;
    return handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode);
#else
    return isatty(fileno(stream)) != 0;
#endif
}

int terminalWidth(FILE* stream) {
#ifdef Q_OS_WIN
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream)));
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (handle != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(handle, &info))
        return qMax(1, static_cast<int>(info.srWindow.Right - info.srWindow.Left + 1));
#else
    winsize size{};
    if (ioctl(fileno(stream), TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
        return static_cast<int>(size.ws_col);
#endif
    return 80;
}

QString safeTerminalText(QString value) {
    QString clean;
    clean.reserve(qMin(value.size(), 320));
    for (const QChar ch : value) {
        const ushort code = ch.unicode();
        if (code < 0x20 || (code >= 0x7f && code <= 0x9f)) continue;
        clean.append(ch);
        if (clean.size() >= 300) {
            clean.append(QChar(0x2026));
            break;
        }
    }
    return clean;
}

QString phaseLabel(const QString& phase) {
    if (phase == "select") return QStringLiteral("выбор сервера измерения");
    if (phase == "ping") return QStringLiteral("задержка");
    if (phase == "download") return QStringLiteral("загрузка");
    if (phase == "upload") return QStringLiteral("отдача");
    if (phase == "connection") return QStringLiteral("данные подключения");
    return QStringLiteral("операция");
}

QString phaseStatusLabel(const QString& status) {
    if (status == "ok") return QStringLiteral("готово");
    if (status == "error") return QStringLiteral("ошибка");
    if (status == "canceled") return QStringLiteral("остановлено");
    if (status == "skipped") return QStringLiteral("пропущено");
    return QStringLiteral("выполняется");
}

class TerminalProgress {
public:
    explicit TerminalProgress(bool enabled) : enabled_(enabled), width_(terminalWidth(stdout)) {}

    void update(const QString& text) {
        if (!enabled_) return;
        auto line = safeTerminalText(text);
        const int maxLength = qMax(1, width_ - 1);
        if (line.size() > maxLength) {
            line.truncate(qMax(0, maxLength - 1));
            line.append(QChar(0x2026));
        }
        const int width = qMax(displayed_.size(), line.size());
        writeOutput(stdout, (QStringLiteral("\r") + line + QString(width - line.size(), QLatin1Char(' '))).toUtf8());
        displayed_ = line;
    }

    void clear() {
        if (!enabled_ || displayed_.isEmpty()) return;
        writeOutput(stdout, (QStringLiteral("\r") + QString(displayed_.size(), QLatin1Char(' ')) + QStringLiteral("\r")).toUtf8());
        displayed_.clear();
    }

private:
    bool enabled_ = false;
    int width_ = 80;
    QString displayed_;
};

class ConsoleColorMode {
public:
    explicit ConsoleColorMode(bool requested) {
#ifdef Q_OS_WIN
        handle_ = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
        if (!requested || handle_ == INVALID_HANDLE_VALUE || !GetConsoleMode(handle_, &original_)) return;
        enabled_ = SetConsoleMode(handle_, original_ | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
        enabled_ = requested;
#endif
    }

    ~ConsoleColorMode() {
#ifdef Q_OS_WIN
        if (enabled_) SetConsoleMode(handle_, original_);
#endif
    }

    bool enabled() const { return enabled_; }

private:
    bool enabled_ = false;
#ifdef Q_OS_WIN
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    DWORD original_ = 0;
#endif
};

void verboseEvent(const QJsonObject& event) {
    const QString kind = event.value("kind").toString();
    const QString service = puls::displayName(event.value("service").toString());
    const QString phase = phaseLabel(event.value("phase").toString());
    QString line;
    if (kind == "phase_started") {
        line = QStringLiteral("подробно  %1 · %2").arg(service, phase);
    } else if (kind == "connection_started") {
        line = QStringLiteral("подробно  определение данных подключения");
    } else if (kind == "server_selected") {
        const auto server = event.value("server").toObject();
        line = QStringLiteral("подробно  %1 · сервер измерения: %2")
                   .arg(service, safeTerminalText(server.value("name").toString()));
    } else if (kind == "phase_completed") {
        const auto result = event.value("phase_result").toObject();
        line = QStringLiteral("подробно  %1 · %2: %3")
                   .arg(service, phase, safeTerminalText(result.value("status").toString()));
        const auto error = result.value("error").toObject();
        if (!error.isEmpty())
            line += QStringLiteral(" · %1: %2").arg(safeTerminalText(error.value("code").toString()),
                                                       safeTerminalText(error.value("message").toString()));
        for (const auto& warning : result.value("warnings").toArray())
            line += QStringLiteral(" · предупреждение: %1").arg(safeTerminalText(warning.toString()));
    } else if (kind == "connection_completed") {
        const auto connection = event.value("connection").toObject();
        line = QStringLiteral("подробно  данные подключения: %1").arg(safeTerminalText(connection.value("status").toString()));
        const auto error = connection.value("error").toObject();
        if (!error.isEmpty())
            line += QStringLiteral(" · %1: %2").arg(safeTerminalText(error.value("code").toString()),
                                                       safeTerminalText(error.value("message").toString()));
        for (const auto& warning : connection.value("warnings").toArray())
            line += QStringLiteral(" · предупреждение: %1").arg(safeTerminalText(warning.toString()));
    }
    if (!line.isEmpty()) errorOutput(safeTerminalText(line) + '\n');
}

QString selectService() {
    while (true) {
        if (!writeOutput(stdout, QStringLiteral(
                "Puls · сервис измерения\n"
                "  1  Яндекс.Интернетометр\n"
                "  2  speedtest.ru\n"
                "  3  Оба сервиса последовательно\n"
                "Выберите 1–3 [1], q — выход: ").toUtf8())) return {};
        std::string input;
        if (!std::getline(std::cin, input)) return {};
        const auto choice = QString::fromUtf8(input.c_str()).trimmed().toLower();
        if (choice.isEmpty() || choice == "1" || choice == "yandex") return "yandex";
        if (choice == "2" || choice == "speedtest") return "speedtest";
        if (choice == "3" || choice == "all") return "all";
        if (choice == "q" || choice == "quit") return {};
        if (!writeOutput(stdout, QStringLiteral("Введите 1, 2 или 3.\n\n").toUtf8())) return {};
    }
}

int runtimeProbe(int& argc, char** argv) {
    QCoreApplication application(argc, argv);
    const bool ssl = QSslSocket::supportsSsl();
    const QJsonObject result{{"qt_version", QString::fromLatin1(qVersion())},
                             {"ssl_available", ssl}, {"ssl_backend", QSslSocket::activeBackend()},
                             {"plugin_paths", QJsonArray::fromStringList(QCoreApplication::libraryPaths())},
                             {"qt_prefix", QLibraryInfo::path(QLibraryInfo::PrefixPath)}};
    return writeOutput(stdout, QJsonDocument(result).toJson(QJsonDocument::Compact) + '\n') && ssl ? 0 : 1;
}
} // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    QStringList arguments;
    for (int index = 1; index < argc; ++index) arguments.append(QString::fromLocal8Bit(argv[index]));
    // Private deployment check: never adds a public command or changes normal
    // version output. It exercises TLS plugin loading from a relocated package.
    if (qEnvironmentVariable("PULS_RUNTIME_PROBE") == "1" && arguments == QStringList{"version"})
        return runtimeProbe(argc, argv);
    puls::Config config;
#ifndef Q_OS_ANDROID
    try {
        config = puls::parseConfig(arguments);
    } catch (const std::invalid_argument& error) {
        errorOutput(QStringLiteral("Ошибка: %1\nПодсказка: puls help\n").arg(QString::fromUtf8(error.what())));
        return 2;
    }
#else
    config.command = puls::Command::GUI;
#endif
    if (config.command == puls::Command::Help || config.command == puls::Command::Version) {
        const auto text = config.command == puls::Command::Help ? puls::helpText() : puls::versionText();
        if (!writeOutput(stdout, text.toUtf8())) {
            errorOutput(QStringLiteral("ошибка записи результата\n"));
            return 1;
        }
        return 0;
    }
    if (config.command == puls::Command::GUI) {
#ifdef PULS_BUILD_GUI
#ifdef Q_OS_WIN
        DWORD consoleProcesses[2]{};
        if (GetConsoleProcessList(consoleProcesses, 2) == 1 &&
            consoleProcesses[0] == GetCurrentProcessId())
            FreeConsole();
#endif
        return puls::runGUI(argc, argv, config);
#else
        errorOutput(QStringLiteral("графический интерфейс недоступен в этой сборке\n"));
        return 2;
#endif
    }
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("Puls"));
    if (config.command == puls::Command::Measure && config.service.isEmpty()) {
        if (!config.json && terminal(stdin) && terminal(stdout)) {
            config.service = selectService();
            if (config.service.isEmpty()) return 130;
            config.explicitService = true;
        } else config.service = "yandex";
    }
    puls::Cancellation cancellation;
    SignalMonitor monitor(cancellation);
    const bool wantsColor = !config.json && !config.noColor && terminal(stdout) &&
                            std::getenv("NO_COLOR") == nullptr && qEnvironmentVariable("TERM") != "dumb";
    TerminalProgress progress(!config.json && terminal(stdout));
    try {
        puls::Observer observer;
        if (config.verbose || (!config.json && terminal(stdout))) observer = [&](const QJsonObject& event) {
            const auto kind = event.value("kind").toString();
            if (config.verbose) {
                progress.clear();
                verboseEvent(event);
            }
            if (config.json || !terminal(stdout)) return;
            const auto service = puls::displayName(event.value("service").toString());
            const auto phase = phaseLabel(event.value("phase").toString());
            if (kind == "connection_started") progress.update(QStringLiteral("Определение данных подключения…"));
            else if (kind == "connection_completed") progress.clear();
            else if (kind == "phase_started") progress.update(QStringLiteral("%1 · %2…").arg(service, phase));
            else if (kind == "server_selected") {
                const auto server = event.value("server").toObject();
                progress.update(QStringLiteral("%1 · сервер измерения: %2").arg(service, safeTerminalText(server.value("name").toString())));
            } else if (kind == "throughput_progress") {
                const auto throughput = event.value("throughput").toObject();
                const auto speed = throughput.value("mbps");
                const auto value = speed.isDouble() ? QStringLiteral("%1 Мбит/с").arg(speed.toDouble(), 0, 'f', 2)
                                                    : QStringLiteral("расчёт скорости…");
                progress.update(QStringLiteral("%1 · %2 · %3").arg(service, phase, value));
            } else if (kind == "phase_completed") {
                const auto result = event.value("phase_result").toObject();
                progress.update(QStringLiteral("%1 · %2: %3")
                                    .arg(service, phase, phaseStatusLabel(result.value("status").toString())));
            }
        };
        const auto result = puls::Runner().run(config, cancellation, observer);
        progress.clear();
        ConsoleColorMode colorMode(wantsColor);
        const auto output = config.json ? QJsonDocument(result).toJson(QJsonDocument::Indented)
                                        : puls::renderHuman(result, colorMode.enabled()).toUtf8();
        if (!writeOutput(stdout, output)) {
            errorOutput(config.json ? QStringLiteral("ошибка записи JSON\n") : QStringLiteral("ошибка записи результата\n"));
            return 1;
        }
        if (cancellation.canceled() || interrupted.load()) {
            errorOutput(QStringLiteral("операция прервана\n"));
            return 130;
        }
        return puls::exitCode(result);
    } catch (const std::exception& error) {
        progress.clear();
        errorOutput(QStringLiteral("ошибка: %1\n").arg(QString::fromUtf8(error.what())));
        return cancellation.canceled() || interrupted.load() ? 130 : 1;
    }
}
