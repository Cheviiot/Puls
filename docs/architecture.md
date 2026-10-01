# Архитектура

Puls имеет два интерфейса над одним application-слоем:

```text
CLI ─┐
     ├─ application ─ service ─ Яндекс / speedtest.ru
GUI ─┘                └ measure
```

- `application` управляет выбором сервиса, фазами, fallback и формирует единый
  результат.
- GUI содержит интерфейс, тему и только безопасные настройки.
- `service` определяет `Backend`, типизированные ошибки и сетевые контракты;
  вложенные модули реализуют first-party протоколы сервисов.
- `measure` отвечает за workers, reconnect, deadline и подтверждённые байты.
- `scripts/release.py` упаковывает архивы выпуска и собирает manifest и
  checksums; APK собирается из той же C++ версии с Qt for Android.

Измерение выполняется последовательно: `select → ping → download → upload`.
Ошибка отдельного сервиса не останавливает `all`. GUI получает immutable
события runner и обновляет widgets только в UI-потоке.

Инварианты: timer начинается после ready, warm-up не учитывается, принимаются
только проверенные ответы и подтверждённые байты, worker имеет один reconnect,
а cancellation закрывает I/O. Сетевой сбой никогда не становится успешным
нулевым результатом.

GUI сохраняет тему, сервис, профиль, длительность, число соединений, выбранную
фазу и размер окна. IP, ISP, сервер, результаты, diagnostics и credentials на
диск не записываются.

## Переход на C++

Проект переносится с Go на C++20 с CMake и vcpkg. C++ версия уже содержит
CLI, GUI на Qt Quick, application runner, engine, оба протокола, сборку
выпуска и Android-приложения; Go-версия удаляется следующим этапом.

| Слой | C++ | Go |
| --- | --- | --- |
| CLI | `src/puls/cli` | `cmd/puls` |
| Application | `src/puls/application` | `internal/application` |
| Engine | `src/puls/measure` | `internal/measure` |
| Общие контракты | `src/puls/service` | `internal/service` |
| Яндекс.Интернетометр | `src/puls/service/yandex` | `internal/service/yandex` |
| speedtest.ru | `src/puls/service/speedtestru` | `internal/service/speedtestru` |
| Терминал | `src/puls/ui` | `internal/ui` |
| HTTP, WebSocket, TLS | `src/puls/net` | стандартная библиотека Go |
| Context, ошибки, JSON, IP | `src/puls/core` | стандартная библиотека Go |
| GUI | `src/puls/gui` | `internal/gui` |
| Сборка Android | Qt for Android, `cmake/android-toolchain.cmake` | — |
| Сборка выпуска | `scripts/release.py`, CMake install | — |

Пока обе реализации существуют, изменения протоколов, CLI и JSON вносятся в
обе либо фиксируются ниже как расхождение.

### Устройство C++ версии

- `core::Context` и `CancelScope` повторяют семантику Go `context`: отмена,
  deadline и callbacks, которые закрывают сетевые операции.
- `Error` хранит дерево причин с тегами и типизированными деталями — аналог
  `errors.Is/As`; `Result<T>` возвращает значение или ошибку. Сетевые и
  протокольные ошибки не передаются исключениями.
- `net` предоставляет синхронный HTTP/1.1 и WebSocket поверх Boost.Beast и
  OpenSSL. Каждая сессия владеет собственным `io_context`; отмена закрывает
  сокет, DNS не блокирует отмену, соединение использует Happy Eyeballs.
- TLS 1.2+ проверяет цепочку, имя хоста или IP и использует системное
  хранилище: bundle Linux/BSD, anchors и trust settings macOS, хранилища
  `ROOT`/`CA` Windows и все сертификаты обновляемого хранилища Android 14+
  (`/apex/com.android.conscrypt/cacerts`) или системного
  (`/system/etc/security/cacerts`): их имена посчитаны хешем OpenSSL 0.9.8,
  который не находит поиск по хешу в OpenSSL 3.
- `measure::run` запускает поток на worker и сохраняет инварианты engine.
- GUI построен на Qt 6 Quick (QML, стиль Material). Состояние, тексты и
  правила dashboard находятся в Qt-независимой `gui::Dashboard` и
  тестируются во всех конфигурациях; `DashboardController` запускает runner в
  рабочем потоке и передаёт события в UI-поток queued-вызовами. Настройки
  хранятся в `QSettings` с теми же ключами, что и в Go-версии.
- GUI — отдельная программа `puls-gui` (на macOS — `Puls.app`), поэтому CLI
  не зависит от Qt и графических библиотек системы. `puls gui` ищет её в
  каталоге `puls`, на macOS также в `~/Applications` и `/Applications`, и на
  Unix заменяет ею свой процесс, а на Windows запускает её, не дожидаясь
  завершения, как другие графические программы.
- Все зависимости из vcpkg, включая Qt, линкуются статически, на Windows
  вместе с CRT: архив выпуска содержит готовые программы без DLL и Visual C++
  Redistributable. Qt используется по LGPL 3: исходный код Puls открыт, и
  программу можно пересобрать с изменённым Qt; `THIRD_PARTY_NOTICES.txt` в
  архиве перечисляет лицензии зависимостей.
- Linux-сборка с GUI выполняется в контейнере manylinux_2_28 (AlmaLinux 8,
  GCC из gcc-toolset), поэтому программам достаточно glibc 2.28 и
  системных библиотек X11. Qt в ней содержит только платформу X11 (в
  сеансах Wayland окно работает через XWayland) и по умолчанию рисует
  программным рендерером, не завися от драйверов OpenGL.
- Overlay-порт `cmake/ports/libuuid` намеренно пуст: порт fontconfig в vcpkg
  объявляет зависимость от libuuid, но fontconfig 2.17 её не использует, а
  исходники libuuid загружаются с ненадёжного SourceForge.
- На Linux и macOS зависимости vcpkg собираются только в Release. MSVC не
  смешивает отладочную и обычную CRT, поэтому на Windows presets
  `windows-release` и `windows-arm64-release` используют triplets
  `*-windows-static-release`, а `windows-debug` собирает обе конфигурации.
- `PULS_PORTABLE_INSTALL=ON` превращает `cmake --install` в раскладку архива
  выпуска: программы, документы, `THIRD_PARTY_NOTICES.txt` и значок для
  ярлыка Linux в корне каталога. Без этого параметра установка следует
  GNUInstallDirs и добавляет `.desktop`-файл и значок для пакетов
  дистрибутивов.
- На macOS у qtbase включена функция `dnslookup`: без неё Qt 6.11 использует
  libresolv в `QHostInfo`, но не линкует её.
- JSON schema 1 кодируется побайтно совместимо с Go (`encoding/json` с
  отступом в два пробела и HTML-экранированием).
- Тесты используют GoogleTest и локальные HTTPS/WSS серверы с временными
  сертификатами; live tests собираются только с `PULS_LIVE_TESTS=ON`.

### Отличия от Go-версии

- Используется только HTTP/1.1: каждый поток измерения получает отдельное
  TCP-соединение, HTTP/2 multiplexing не применяется.
- Proxy из окружения не используется, включая WebSocket Яндекса.
- Ошибки флагов, которые Go-версия выводила по-английски из пакета `flag`
  (`bad flag syntax`, `invalid boolean value`), выводятся по-русски.
- Windows: OpenSSL видит только корневые сертификаты, уже установленные в
  хранилище; автоматическая загрузка отсутствующих корней Windows не
  выполняется.
- Минимальная macOS — 13.3 из-за `std::to_chars` для чисел с плавающей точкой.
- Графический интерфейс — отдельная программа `puls-gui`; `puls gui`
  запускает её. На Windows `puls gui` возвращается сразу после запуска окна.
- Выпуск для Linux собирается с Qt только для X11; в сеансах Wayland окно
  работает через XWayland.
