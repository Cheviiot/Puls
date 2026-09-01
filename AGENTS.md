# Инструкции для AI-разработчиков Puls

Область действия этого файла — весь репозиторий. Более вложенный `AGENTS.md`
может уточнять правила только для своего каталога, но не должен ослаблять
требования к корректности измерений, безопасности и приватности.

## Контекст продукта

Puls — кроссплатформенный CLI для измерения задержки, скорости скачивания и
отдачи. Продукт ориентирован на русскоязычных пользователей, а его машинные
интерфейсы следуют привычным Unix- и Go-конвенциям.

Поддерживаемые источники:

- `yandex` — Яндекс.Интернетометр;
- `speedtest` — speedtest.ru;
- `all` — последовательный запуск всех поддерживаемых источников.

Добавление нового источника — продуктовое решение. Не добавляй сторонний сервис
только потому, что его проще интегрировать.

## Язык и интерфейс

- Пользовательские описания, ошибки, прогресс и справка должны быть на русском.
- Команды, flags, values, environment variables, JSON fields, status values,
  package names, symbols и code comments остаются на английском.
- Не создавай кириллические параметры командной строки.
- Названия брендов, URL, protocol commands и HTTP headers не переводи.
- Формулируй сообщения коротко и конкретно: что сломалось, на какой фазе и что
  пользователь может сделать.
- Обычный вывод предназначен человеку. `--json` предназначен программе и не
  должен содержать decoration, progress или ANSI escape sequences.
- `stdout` содержит результат. Диагностика `--verbose` и предупреждения,
  способные нарушить JSON, выводятся в `stderr`.

Синтаксис CLI:

```text
puls [source] [options]
```

Актуальные public options:

```text
--profile quick|balanced|accurate
--duration 3..60
--connections 0..16
--only all|ping|download|upload
--server <host>
--json
--verbose
--no-color
--version
-h, --help
```

`--connections=0` означает нативный автоматический режим. `--server` относится
только к `speedtest`. Не добавляй aliases и скрытые legacy flags без явной
продуктовой причины.

## Карта репозитория

- `cmd/puls` — parsing CLI, orchestration, signals, exit codes и output models;
- `cmd/release` — воспроизводимая кроссплатформенная упаковка релизов;
- `internal/measure` — общий concurrency engine для throughput;
- `internal/provider` — общий provider contract и typed errors;
- `internal/provider/yandex` — протокол Яндекс.Интернетометра;
- `internal/provider/speedtestru` — протокол speedtest.ru;
- `internal/ui` — terminal detection, colors, progress и финальный вывод;
- `docs` — архитектура и воспроизводимая дистрибуция;
- `scripts/install.sh`, `scripts/install.ps1` — исходники проверяемых установщиков;
- `.github` — CI, release workflow и шаблоны проекта.

Не переноси protocol-specific знания в `cmd/puls` или `internal/measure`.
Общая механика потоков должна оставаться независимой от конкретного источника.

## Неизменяемые правила измерения

Любое изменение engine или provider обязано сохранять эти свойства:

1. Учитываемый таймер запускается только после готовности хотя бы одного worker.
2. Warm-up завершается до старта таймера, его байты не входят в результат.
3. Учитываются только байты, подтверждённые протоколом или успешным HTTP-ответом.
4. HTTP response принимается только при ожидаемом status `2xx`, корректной schema
   и допустимом payload.
5. WebSocket message type, protocol command и payload size проверяются до учёта
   данных.
6. HTTP compression для download отключена, чтобы считать переданные байты, а
   не результат распаковки.
7. Ошибки worker агрегируются. Если все потоки завершились с ошибкой, фаза
   заканчивается раньше deadline и возвращает ошибку, а не `0 Мбит/с`.
8. Worker может один раз переподключиться. Не перезапускай throughput phase
   целиком после частично выполненного замера.
9. Частичный успех явно отражается в result status, stream counters и warnings.
10. Отмена context останавливает network I/O и goroutines; Ctrl+C должен
    завершать процесс не дольше чем примерно за одну секунду.
11. `all` продолжает запуск после ошибки отдельного provider и возвращает
    совокупный partial/error result.
12. `duration` ограничена диапазоном 3–60 секунд, ручное число соединений —
    1–16, автоматический режим — не более 16.

Не исправляй подозрительный результат коэффициентом, искусственным clamp или
заменой ошибки нулём. Исправляй выбор endpoint, time boundaries, byte accounting
или protocol validation.

## Правила протоколов

### Общие

- Используй только публично поставляемые first-party endpoints и browser
  credentials официального клиента.
- Не обходи CAPTCHA, anti-bot, browser challenge, rate limit или access control.
- Не добавляй telemetry, perf reporting и отправку результатов измерения обратно
  сервису.
- Не записывай JWT, IP-related responses или другие runtime credentials на диск.
- Никогда не печатай token, credential или полный чувствительный response в
  `--verbose` и error messages.
- Discovery и ping можно повторить не более двух раз. Повтор должен быть
  ограничен context deadline.
- Изменение undocumented protocol сначала сверяй с актуальным first-party
  frontend или документацией. Ссылку и дату проверки фиксируй в PR description,
  test comment или документации.
- Не используй third-party proxy для восстановления недоступного API.

### Yandex

- Получай актуальные CDN через `get-probes`; проверяй обязательные probes и URL.
- Выполняй четыре latency requests к каждому CDN параллельно и выбирай
  минимальный валидный RTT согласно browser client.
- Распределяй download streams между ближайшими CDN.
- Для upload предпочитай WebSocket и учитывай только подтверждения вида
  `{"k":"u","b":...}`.
- При недоступности WebSocket разрешён fallback на `postUrl`; учитывай POST
  только после успешного HTTP status.

### speedtest.ru

- Получай кандидатов из `/api/nearest_servers`, проверяй их и выбирай минимальную
  median latency.
- Получай краткоживущий JWT через `/api/server/gentoken`, храни только в памяти
  и обновляй один раз после `401`.
- При ротации public browser key извлекай актуальное значение из текущего page
  bundle и повторяй запрос один раз.
- Download использует `download.php?ckSize=...`, upload — `upload.php` с header
  `jwt`.
- Сохраняй native параметры: median из 10 ping samples, официальный jitter,
  adaptive download chunk и upload blocks по 2 MiB.
- Fallback server list подходит только для ping. Без discovery/JWT throughput
  возвращает явную authorization error.

## Контракт результатов

`PingResult` должен сохранять основной `ValueMs`, min/median/average/jitter,
число samples и method. `ThroughputResult` должен сохранять Mbps, подтверждённые
bytes, elapsed, successful/failed streams и warnings.

Ошибки provider должны оставаться типизированными и включать:

- `provider`;
- `phase`;
- стабильный `code`;
- `retryable`;
- исходный `cause` через Go error wrapping.

JSON сохраняет верхнеуровневые поля `provider`, `server`, `status`, `ping_ms`,
`jitter_ms`, `download_mbps`, `upload_mbps`, `phases`, `error_code`, `error` и
`warnings`. Отсутствующие измерения выводятся как `null`, а не удаляются через
`omitempty`.

Exit codes:

- `0` — success;
- `1` — provider error, partial result или JSON write error;
- `2` — invalid CLI arguments;
- `130` — canceled by signal.

До первого публичного стабильного релиза контракт можно менять осознанно, но
изменение CLI или JSON требует синхронного обновления help, README, golden tests
и CHANGELOG. После стабильного релиза считай CLI и JSON public API.

## Порядок работы AI-агента

1. Прочитай применимые `AGENTS.md`, затем проверь `git status --short`.
2. Найди реализацию и связанные тесты через `rg`; не делай выводы только по
   названию файла.
3. Зафиксируй ожидаемое поведение тестом или явно определи проверяемый invariant.
4. Вноси минимально достаточное изменение и не перезаписывай несвязанные правки
   пользователя.
5. Форматируй изменённый Go-код через `gofmt`.
6. Сначала запускай targeted tests, затем обязательные проверки репозитория.
7. При изменении поведения обновляй help, README и CHANGELOG в том же change set.
8. Перед завершением перечитай diff, проверь отсутствие secrets и generated
   artifacts, затем сообщи точные результаты проверок.

Не используй destructive Git commands. Не удаляй и не откатывай пользовательские
изменения. Не коммить бинарный файл `puls`, каталог `dist/`, токены, capture с
персональными данными или ответы production API.

## Тестирование

Для каждого изменения выполняй подходящий минимум:

```bash
go test ./...
go test -race ./...
go vet ./...
```

Перед релизом обязательна полная проверка:

```bash
go test ./...
go test -count=10 ./...
go test -race ./...
go vet ./...
staticcheck ./...
govulncheck ./...
actionlint .github/workflows/*.yml
shellcheck scripts/install.sh
```

Не устанавливай отсутствующие system tools на host без необходимости. Следуй
локальным инструкциям окружения: project-local dependencies предпочтительны,
toolchain-specific packages устанавливаются в Distrobox.

После изменения `scripts/install.ps1` проверь его в `puls-powershell-dev` по команде из
`docs/distribution.md`. Функциональный Windows-сценарий остаётся обязательным CI
test и не заменяется одной syntax-проверкой.

### Mock tests

Network protocol должен тестироваться локальными `httptest`/WebSocket mocks без
зависимости от интернета. В зависимости от затронутого кода покрой:

- success path и точный byte count;
- malformed JSON/schema/frame/message type;
- `401`, `403`, `5xx` и неожиданный success payload;
- disconnect, timeout, cancellation и одно reconnect;
- partial streams и early failure всех workers;
- запуск таймера после ready и исключение warm-up;
- отсутствие goroutine leaks;
- browser key/JWT rotation;
- Yandex WebSocket → HTTP fallback;
- JSON `null`, statuses и exit codes.

Тест не должен принимать реальный сетевой сбой за ожидаемый success.

### Live tests

Live tests всегда изолируются build tag `live`:

```bash
go test -tags=live ./internal/provider/...
```

Discovery и ping могут запускаться без дополнительного opt-in. Download/upload
разрешены только при явной переменной:

```bash
PULS_LIVE_THROUGHPUT=1 go test -tags=live -run Live ./internal/provider/...
```

Не запускай throughput live test без необходимости: он расходует трафик и
нагружает публичную инфраструктуру. Никогда не превращай live test в обычный CI
test.

## Кроссплатформенность и релизы

- Production binary должен собираться с `CGO_ENABLED=0`, если изменение не
  обосновывает обратное.
- Не используй OS-specific syscalls вне файлов с корректными build constraints.
- Пути создавай через `path/filepath`, окончания строк и executable bits
  проверяй на целевых системах.
- Базовые release targets: Linux, Windows и macOS для `amd64` и `arm64`.
- Архивы создавай через `cmd/release`, а не набором ручных shell-команд.
- Release archive содержит binary, `README.md`, `CHANGELOG.md`, `LICENSE` и
  полный комплект публичной Markdown-документации.
- Версия внедряется build flags и должна совпадать с Git tag без префикса `v` в
  выводе приложения.
- Для новых targets используй `--targets`; не ухудшай базовую матрицу.
- Установщики получают assets только из GitHub Releases, проверяют точный
  SHA-256 до распаковки и по умолчанию не требуют прав администратора.

Проверка сборщика:

```bash
go run ./cmd/release --version 0.0.0-test
```

Удаляй локальный `dist/` после проверки только если его создал текущий change и
он не содержит пользовательских файлов.

## Definition of Done

Работа завершена только если:

- реализация соответствует запросу и правилам измерения;
- ошибки не превращаются в успешные нулевые значения;
- добавлены или обновлены relevant unit/mock/golden tests;
- `go test ./...`, `go test -race ./...` и `go vet ./...` прошли;
- документация и help совпадают с реальным CLI;
- JSON и exit codes проверены при изменении output path;
- Ctrl+C и cleanup проверены при изменении concurrency/network code;
- direct installers проверяют SHA-256 и покрыты integration tests при их изменении;
- diff не содержит secrets, случайных binary artifacts и несвязанных правок;
- итоговый отчёт перечисляет изменённое, проверки и оставшиеся ограничения.
