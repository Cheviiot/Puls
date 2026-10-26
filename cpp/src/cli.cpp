#include "puls/core.hpp"

#include <QRegularExpression>
#include <QTextStream>
#include <utility>

#ifndef PULS_VERSION
#define PULS_VERSION "cpp-dev"
#endif

namespace puls {
namespace {
[[noreturn]] void usage(const QString& message) {
    throw std::invalid_argument(message.toUtf8().constData());
}

QString normalized(const QString& value) { return value.trimmed().toLower(); }

bool isService(const QString& value) {
    return value == "yandex" || value == "speedtest" || value == "all";
}

int integer(const QString& value, const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[+-]?[0-9]+$"));
    bool ok = false;
    const int result = value.toInt(&ok);
    if (!ok || !pattern.match(value).hasMatch()) {
        if (name == "duration")
            usage(QStringLiteral("неверная длительность «%1»: требуется целое число секунд").arg(value));
        usage(QStringLiteral("неверное значение «%1» для параметра --%2: требуется целое число").arg(value, name));
    }
    return result;
}

bool boolean(const QString& value, const QString& name) {
    if (value == "1" || value == "t" || value == "T" || value == "true" || value == "TRUE" || value == "True")
        return true;
    if (value == "0" || value == "f" || value == "F" || value == "false" || value == "FALSE" || value == "False")
        return false;
    usage(QStringLiteral("неверное значение «%1» для параметра --%2: требуется true или false").arg(value, name));
}

QString valueOrDash(const QJsonValue& value) {
    const auto text = value.toString();
    return text.isEmpty() ? QStringLiteral("—") : text;
}

QString statusText(const QString& status) {
    if (status == "ok") return QStringLiteral("готово");
    if (status == "partial") return QStringLiteral("частичный результат");
    if (status == "canceled") return QStringLiteral("остановлено");
    return QStringLiteral("измерение не выполнено");
}

QString metric(const QJsonObject& phase, const QString& field, const QString& unit) {
    const auto value = phase.value(field);
    if (value.isDouble()) return QStringLiteral("%1 %2").arg(value.toDouble(), 0, 'f', 2).arg(unit);
    if (phase.value("status") == "skipped") return QStringLiteral("—");
    const auto message = phase.value("error").toObject().value("message").toString();
    return message.isEmpty() ? QStringLiteral("—") : QStringLiteral("не удалось · ") + message;
}
} // namespace

Config parseConfig(const QStringList& args) {
    Config config;
    if (args.isEmpty()) return config;
    int index = 0;
    if (!args.first().startsWith('-')) {
        const auto command = normalized(args.first());
        if (command == "help" || command == "version") {
            if (args.size() != 1)
                usage(QStringLiteral("команда %1 не принимает аргументы").arg(command));
            config.command = command == "help" ? Command::Help : Command::Version;
            return config;
        }
        if (command == "gui") config.command = Command::GUI;
        else if (command == "ip") config.command = Command::IP;
        else if (isService(command)) {
            config.service = command;
            config.explicitService = true;
        } else usage(QStringLiteral("неизвестная команда «%1»; выберите yandex, speedtest, all или ip").arg(args.first()));
        ++index;
        if (config.command == Command::IP && index < args.size() && !args[index].startsWith('-')) {
            const auto service = normalized(args[index++]);
            if (!isService(service) || service == "all")
                usage(QStringLiteral("неизвестный сервис «%1»; выберите speedtest или yandex").arg(args[index - 1]));
            config.service = service;
            config.explicitService = true;
        }
    }

    bool showHelp = false, shortHelp = false, showVersion = false;
    QString duration;
    QString extra;
    for (; index < args.size(); ++index) {
        const auto argument = args[index];
        if (argument == "--") {
            if (++index < args.size()) extra = args[index];
            break;
        }
        if (!argument.startsWith('-') || argument == "-") {
            extra = argument;
            break;
        }
        auto name = argument.mid(argument.startsWith("--") ? 2 : 1);
        const auto equals = name.indexOf('=');
        const bool hasValue = equals >= 0;
        const auto inlineValue = hasValue ? name.mid(equals + 1) : QString();
        if (hasValue) name.truncate(equals);
        bool* flag = nullptr;
        if (name == "help") flag = &showHelp;
        else if (name == "h") flag = &shortHelp;
        else if (name == "version") flag = &showVersion;
        else if (name == "verbose") flag = &config.verbose;
        else if (config.command != Command::GUI && name == "json") flag = &config.json;
        else if (config.command != Command::GUI && name == "no-color") flag = &config.noColor;
        else if (config.command == Command::Measure && name == "show-ip") flag = &config.showIP;
        if (flag) {
            *flag = hasValue ? boolean(inlineValue, name) : true;
            continue;
        }
        const bool valueFlag = config.command == Command::Measure &&
            (name == "profile" || name == "duration" || name == "connections" || name == "only" || name == "server");
        if (!valueFlag) usage(QStringLiteral("неизвестный параметр: --%1").arg(name));
        if (!hasValue && index + 1 == args.size())
            usage(QStringLiteral("параметр требует значение: --%1").arg(name));
        const auto value = hasValue ? inlineValue : args[++index];
        if (name == "profile") config.profile = value;
        else if (name == "duration") duration = value;
        else if (name == "connections") config.connections = integer(value, name);
        else if (name == "only") config.only = value;
        else if (name == "server") config.server = value;
    }
    if (showHelp || shortHelp) { config.command = Command::Help; return config; }
    if (showVersion) { config.command = Command::Version; return config; }
    if (!extra.isNull()) usage(QStringLiteral("неизвестный аргумент «%1»").arg(extra));
    if (config.command != Command::Measure) return config;
    const auto profile = normalized(config.profile);
    if (profile == "quick") config.durationSeconds = 5;
    else if (profile == "balanced") config.durationSeconds = 10;
    else if (profile == "accurate") config.durationSeconds = 15;
    else usage(QStringLiteral("неизвестный профиль «%1»; выберите quick, balanced или accurate").arg(config.profile));
    config.profile = profile;
    if (!duration.isEmpty()) config.durationSeconds = integer(duration, "duration");
    if (config.durationSeconds < 3 || config.durationSeconds > 60)
        usage(QStringLiteral("длительность должна быть от 3 до 60 секунд"));
    if (config.connections < 0 || config.connections > 16)
        usage(QStringLiteral("число соединений должно быть 0 (авто) или от 1 до 16"));
    config.only = normalized(config.only);
    if (config.only != "all" && config.only != "ping" && config.only != "download" && config.only != "upload")
        usage(QStringLiteral("неизвестный этап «%1»; выберите all, ping, download или upload").arg(config.only));
    if (!config.server.isEmpty() && config.service != "speedtest")
        usage(QStringLiteral("--server можно использовать только с сервисом speedtest"));
    return config;
}

QString versionText() { return QStringLiteral("Puls ") + QString::fromUtf8(PULS_VERSION) + '\n'; }

QString helpText() {
    return QString::fromUtf8(R"(Puls  проверка скорости и качества интернета
Яндекс.Интернетометр и speedtest.ru

ИСПОЛЬЗОВАНИЕ
  puls [service] [options]
  puls ip [service] [options]
  puls gui [options]

СЕРВИСЫ
  yandex                       Яндекс.Интернетометр
  speedtest                    speedtest.ru
  all                          оба сервиса последовательно

КОМАНДЫ
  ip [service]                 внешний IP и интернет-провайдер
  gui                          открыть графический интерфейс
  help                         показать справку
  version                      показать версию

ПАРАМЕТРЫ ИЗМЕРЕНИЯ
  --profile <name>             quick, balanced или accurate
  --duration <seconds>         время фазы · от 3 до 60 секунд
  --connections <number>       0 — автоматически, вручную — от 1 до 16
  --only <phase>               all, ping, download или upload
  --server <host>              сервер измерения · только speedtest
  --show-ip                    добавить данные подключения

ОБЩИЕ ПАРАМЕТРЫ
  --json                       машинный JSON без оформления
  --verbose                    подробная диагностика в stderr
  --no-color                   отключить цвета
  -h, --help                   показать справку
  --version                    показать версию

ПРИМЕРЫ
  puls                         выбрать сервис
  puls gui                     открыть графический интерфейс
  puls yandex                  полная проверка через Яндекс
  puls all --profile quick      быстро проверить оба сервиса
  puls speedtest --only ping    измерить только задержку
  puls ip                      определить IP с резервным сервисом
  puls ip speedtest             IP и интернет-провайдер через speedtest.ru
  puls ip yandex --json         использовать конкретный сервис

C++ dev: измерение загрузки и отдачи пока недоступно.
Используйте --only ping или puls ip.

Ctrl+C останавливает операцию · подробнее: README.md
)");
}

QString renderHuman(const QJsonObject& envelope) {
    QString text;
    QTextStream out(&text);
    if (envelope.value("connection").isObject()) {
        const auto connection = envelope.value("connection").toObject();
        out << "Puls · Подключение\n";
        if (connection.value("status") == "ok") {
            out << "  Внешний IP             " << valueOrDash(connection.value("external_ip")) << '\n';
            out << "  Интернет-провайдер      " << valueOrDash(connection.value("isp")) << '\n';
            if (connection.value("detected_by").isString())
                out << "  Сервис                 " << displayName(connection.value("detected_by").toString()) << '\n';
        } else {
            const auto error = connection.value("error").toObject();
            out << "  Результат              " << (connection.value("status") == "canceled" ? "остановлено" : "не удалось");
            if (!error.value("message").toString().isEmpty()) out << " · " << error.value("message").toString();
            out << '\n';
        }
        for (const auto warning : connection.value("warnings").toArray()) out << "  ! " << warning.toString() << '\n';
        out << '\n';
    }
    const auto results = envelope.value("results").toArray();
    for (const auto value : results) {
        const auto result = value.toObject();
        out << "Puls · " << displayName(result.value("service").toString()) << '\n';
        const auto server = result.value("server").toObject();
        if (!server.isEmpty()) {
            out << "  Сервер измерения       " << valueOrDash(server.value("name"));
            if (!server.value("city").toString().isEmpty()) out << " · " << server.value("city").toString();
            if (!server.value("region").toString().isEmpty()) out << ", " << server.value("region").toString();
            out << '\n';
        }
        const auto phases = result.value("phases").toObject();
        const auto ping = phases.value("ping").toObject();
        out << "  Задержка               " << metric(ping, "value_ms", QStringLiteral("мс"));
        if (ping.value("jitter_ms").isDouble())
            out << "  ·  джиттер " << QString::number(ping.value("jitter_ms").toDouble(), 'f', 2) << " мс";
        out << '\n';
        out << "  Загрузка               " << metric(phases.value("download").toObject(), "mbps", QStringLiteral("Мбит/с")) << '\n';
        out << "  Отдача                 " << metric(phases.value("upload").toObject(), "mbps", QStringLiteral("Мбит/с")) << '\n';
        const auto error = result.value("error").toObject().value("message").toString();
        if (!error.isEmpty()) out << "  Ошибка                 " << error << '\n';
        for (const auto warning : result.value("warnings").toArray()) out << "  ! " << warning.toString() << '\n';
        out << "  " << statusText(result.value("status").toString()) << "\n\n";
    }
    if (results.size() > 1) out << "Итог  " << statusText(envelope.value("status").toString()) << '\n';
    return text;
}
} // namespace puls
