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
- Release builder собирает native GUI/CLI архивы, APK, manifest и checksums.

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
CLI, application runner, engine и оба протокола; GUI, Android-приложение и
release builder пока остаются в Go и переносятся следующими этапами.

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
| GUI и Android | — | `internal/gui` |
| Release builder | — | `cmd/release` |

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
  хранилище: bundle Linux/BSD/Android, anchors и trust settings macOS,
  хранилища `ROOT`/`CA` Windows.
- `measure::run` запускает поток на worker и сохраняет инварианты engine.
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
