# Архитектура

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
