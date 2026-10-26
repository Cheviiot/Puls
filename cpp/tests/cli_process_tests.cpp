#include "puls/core.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <iostream>

#ifndef TEST_DATA_DIR
#error TEST_DATA_DIR must point to the cpp/tests directory
#endif

namespace {
int failures = 0;
QString binary;

void expect(bool condition, const QString& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message.toUtf8().constData() << '\n';
}

struct Result {
    bool started = false;
    bool finished = false;
    QProcess::ExitStatus status = QProcess::CrashExit;
    int code = -1;
    QByteArray output;
    QByteArray error;
};

Result run(const QStringList& arguments, const QString& outputFile = {}) {
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.remove("DISPLAY");
    environment.remove("WAYLAND_DISPLAY");
    environment.remove("PULS_GUI_PREVIEW_DIR");
    environment.insert("QT_QPA_PLATFORM", "puls-invalid-platform-for-cli-test");
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    if (!outputFile.isEmpty()) process.setStandardOutputFile(outputFile);
    process.start(binary, arguments);
    Result result;
    result.started = process.waitForStarted(5000);
    if (result.started) {
        result.finished = process.waitForFinished(5000);
        if (!result.finished) {
            process.kill();
            process.waitForFinished(1000);
        }
    }
    result.status = process.exitStatus();
    result.code = process.exitCode();
    result.output = process.readAllStandardOutput();
    result.error = process.readAllStandardError();
    return result;
}

QString command(const QStringList& arguments) { return arguments.join(' '); }

void assertExit(const Result& result, int code, const QStringList& arguments) {
    const auto label = command(arguments);
    expect(result.started && result.finished, label + ": process starts and exits without a display");
    expect(result.status == QProcess::NormalExit, label + ": process exits normally without a signal");
    expect(result.code == code, label + QStringLiteral(": expected exit %1, got %2; stderr: %3")
                                          .arg(code).arg(result.code).arg(QString::fromUtf8(result.error)));
}

void testHelp(const QByteArray& golden) {
    const QList<QStringList> commands{
        {"help"}, {"--help"}, {"-h"}, {"gui", "--help"}, {"ip", "--help"},
        {"--json=true", "--help"}, {"yandex", "--json=true", "--duration=2", "--help"},
        {"ip", "yandex", "--json=true", "--help"}, {"--help", "--version"}
    };
    for (const auto& arguments : commands) {
        const auto result = run(arguments);
        assertExit(result, 0, arguments);
        expect(result.output == golden, command(arguments) + ": stdout matches the frozen UTF-8 help golden");
        expect(result.error.isEmpty(), command(arguments) + ": successful help leaves stderr empty");
        expect(!result.output.contains('\x1b'), command(arguments) + ": help contains no ANSI without a terminal");
    }
}

void testVersion() {
    const auto expected = puls::versionText().toUtf8();
    for (const auto& arguments : QList<QStringList>{{"version"}, {"--version"}, {"--json=true", "--version"}, {"gui", "--version"}}) {
        const auto result = run(arguments);
        assertExit(result, 0, arguments);
        expect(result.output == expected, command(arguments) + ": version is written only to stdout");
        expect(result.error.isEmpty(), command(arguments) + ": version leaves stderr empty");
        expect(!result.output.contains('\x1b'), command(arguments) + ": version contains no ANSI");
    }
}

void testUsageErrors() {
    const QList<QStringList> commands{
        {"--ip"}, {"--duration", "2"}, {"--duration=61"}, {"--connections=17"},
        {"--connections=-1"}, {"yandex", "--server", "example.com"},
        {"all", "--server=example.com"}, {"ip", "--duration=3"},
        {"ip", "--server=example.com"}, {"ip", "all"}, {"gui", "--json"},
        {"unknown"}, {"help", "--json=true"}, {"version", "extra"},
        {"--json=maybe", "--help"}
    };
    for (const auto& arguments : commands) {
        const auto result = run(arguments);
        assertExit(result, 2, arguments);
        expect(result.output.isEmpty(), command(arguments) + ": usage errors leave stdout empty");
        const auto message = QString::fromUtf8(result.error);
        expect(message.startsWith(QStringLiteral("Ошибка:")) && message.contains(QStringLiteral("Подсказка: puls help")),
               command(arguments) + ": usage errors are Russian and include a help hint on stderr");
        expect(!result.error.contains('\x1b'), command(arguments) + ": stderr has no ANSI without a terminal");
    }
}

void testOutputFailure() {
#ifdef Q_OS_UNIX
    if (!QFileInfo::exists(QStringLiteral("/dev/full"))) return;
    const QStringList arguments{"help"};
    const auto result = run(arguments, QStringLiteral("/dev/full"));
    assertExit(result, 1, arguments);
    expect(result.output.isEmpty(), "write failure does not leak help into captured stdout");
    expect(QString::fromUtf8(result.error).contains(QStringLiteral("ошибка записи результата")),
           "stdout write failure is reported in Russian on stderr");
#endif
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().size() != 2) {
        std::cerr << "Expected path to the puls executable\n";
        return 1;
    }
    binary = QFileInfo(application.arguments()[1]).absoluteFilePath();
    QFile file(QDir(QString::fromUtf8(TEST_DATA_DIR)).filePath("golden/help.txt"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::cerr << "Cannot read frozen help golden\n";
        return 1;
    }
    const auto golden = file.readAll();
    expect(golden.startsWith("Puls ") && golden.endsWith('\n'), "help golden is present and complete");
    testHelp(golden);
    testVersion();
    testUsageErrors();
    testOutputFailure();
    if (failures == 0) std::cout << "CLI process contract checks passed\n";
    return failures == 0 ? 0 : 1;
}
