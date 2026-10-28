#include "puls/core.hpp"
#ifdef PULS_BUILD_GUI
#include "puls/gui.hpp"
#endif

#include <QCoreApplication>
#include <QJsonDocument>
#include <QLibraryInfo>
#include <QSslSocket>
#include <QThread>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
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

QString phaseName(const QString& phase) {
    if (phase == "select") return QStringLiteral("выбор сервера измерения");
    if (phase == "ping") return QStringLiteral("задержка");
    if (phase == "download") return QStringLiteral("загрузка");
    if (phase == "upload") return QStringLiteral("отдача");
    return phase;
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
    try {
        puls::Observer observer;
        if (config.verbose) observer = [](const QJsonObject& event) {
            const auto kind = event.value("kind").toString();
            if (kind == "phase_started")
                errorOutput(QStringLiteral("подробно  %1 · %2\n")
                                .arg(puls::displayName(event.value("service").toString()), phaseName(event.value("phase").toString())));
            else if (kind == "connection_started")
                errorOutput(QStringLiteral("подробно  определение данных подключения\n"));
        };
        const auto result = puls::Runner().run(config, cancellation, observer);
        const auto output = config.json ? QJsonDocument(result).toJson(QJsonDocument::Indented)
                                        : puls::renderHuman(result).toUtf8();
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
        errorOutput(QStringLiteral("ошибка: %1\n").arg(QString::fromUtf8(error.what())));
        return cancellation.canceled() || interrupted.load() ? 130 : 1;
    }
}
