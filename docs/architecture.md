# Архитектура

## C++ / Qt

Новая реализация использует C++20 и Qt Widgets. CLI и GUI вызывают один
`puls::Runner` из `cpp/src/core.cpp`; GUI получает копии событий через очередь
Qt, а сетевые объекты принадлежат потоку Runner. Конкурентный engine находится
в `cpp/src/transfer.cpp`, discovery/ping — в `backend.cpp`, throughput каждого
сервиса — в отдельном файле. CLI/JSON schema 1 и правила учёта подтверждённых
байтов совпадают с reference-контрактом.

GUI создан с нуля: выбор сервиса, адаптивные показатели, отдельные параметры
и закреплённая кнопка запуска/остановки. Настройки сохраняются по whitelist;
данные подключения, результаты и credentials не записываются. Отмена закрывает
I/O; Runner возвращает управление после завершения workers.

Native runtime развёртывается через `cmake/PulsDeployment.cmake`; архивы,
manifest schema 2 и checksums создаёт `scripts/release_cpp.py`. Release workflow
вызывает reusable C++ проверки, desktop и Qt Android workflows. Подробности:
[сборка и проверка C++](cpp-migration.md).

## Go / Fyne reference

Puls имеет два интерфейса над одним application-слоем:

```text
cmd/puls (CLI) ─┐
                ├─ internal/application ─ internal/service ─ Яндекс / speedtest.ru
internal/gui ───┘                         └ internal/measure
```

- `internal/application` управляет выбором сервиса, фазами, fallback и
  формирует единый результат.
- `internal/gui` содержит Fyne-интерфейс, тему и только безопасные настройки.
- `internal/service` определяет `Backend`, типизированные ошибки и сетевые
  контракты; подпакеты реализуют first-party протоколы сервисов.
- `internal/measure` отвечает за workers, reconnect, deadline и подтверждённые
  байты.
- `cmd/release` собирает native GUI/CLI архивы, APK, manifest и checksums.

Измерение выполняется последовательно: `select → ping → download → upload`.
Ошибка отдельного сервиса не останавливает `all`. GUI получает immutable
события runner и обновляет widgets только через `fyne.Do`.

Инварианты: timer начинается после ready, warm-up не учитывается, принимаются
только проверенные ответы и подтверждённые байты, worker имеет один reconnect,
а cancellation закрывает I/O. Сетевой сбой никогда не становится успешным
нулевым результатом.

GUI сохраняет тему, сервис, профиль, длительность, число соединений, выбранную
фазу и размер окна. IP, ISP, сервер, результаты, diagnostics и credentials на
диск не записываются.
