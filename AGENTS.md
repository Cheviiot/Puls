# Инструкции для AI-разработчиков Puls

Файл действует на весь репозиторий. Вложенные инструкции могут уточнять, но не
ослаблять требования к корректности измерений, безопасности и приватности.

## Продукт и язык

Puls — кроссплатформенный CLI для русскоязычных пользователей. Он измеряет
ping, download и upload через два сервиса измерения:

- `yandex` — Яндекс.Интернетометр;
- `speedtest` — speedtest.ru;
- `all` — оба сервиса последовательно.

Терминология обязательна:

- сервис измерения — Яндекс.Интернетометр или speedtest.ru;
- сервер измерения — конкретный CDN/QMS host;
- интернет-провайдер — оператор пользователя;
- endpoint/probe — только код протокола и verbose-диагностика.

Пользовательские сообщения и help пишутся по-русски. Commands, flags, values,
environment variables, JSON fields, Go symbols, package names, code comments и
commit messages остаются английскими. Не добавляй кириллические flags.

## Публичный CLI

```text
puls
puls yandex [options]
puls speedtest [options]
puls all [options]
puls ip [speedtest|yandex] [options]
puls help
puls version
```

Публичные flags:

```text
--profile quick|balanced|accurate
--duration 3..60
--connections 0..16
--only all|ping|download|upload
--server <host>
--show-ip
--json
--verbose
--no-color
--version
-h, --help
```

`--connections=0` — нативный автоматический режим. `--server` допустим только
для отдельного `speedtest`. Удалённый v0.1 flag `--ip` не восстанавливать.

Без аргументов TTY получает выбор сервиса, pipe запускает Yandex. Human output
идёт в `stdout`, verbose — в `stderr`. JSON не содержит ANSI или progress.

## Архитектурные границы

- `cmd/puls`: parsing, configuration, orchestration, results, JSON, rendering;
- `cmd/release`: targets, builder, archives, manifest, checksums, installers;
- `internal/measure`: общий concurrency engine;
- `internal/service`: `Backend`, `ConnectionInfoBackend`, общие types/helpers;
- `internal/service/yandex`: только протокол Яндекса;
- `internal/service/speedtestru`: только протокол speedtest.ru;
- `internal/ui`: TTY, menu, colors, progress;
- `scripts`: direct installers;
- `docs`: архитектура и выпуск.

Передавай logger и внешние зависимости через constructors/options. Не добавляй
global output, setter вида `SetVerbose` или UI в `MeasurementConfig`. Общие
лимиты и преобразования принадлежат `internal/service`; правила конкретного
протокола не выноси туда.

## Инварианты engine

Любое изменение обязано сохранять:

1. Таймер начинается после готовности хотя бы одного worker.
2. Warm-up не входит в учитываемые bytes/elapsed.
3. Считаются только подтверждённые протоколом или успешным HTTP-ответом bytes.
4. Проверяются HTTP 2xx, Content-Type/Encoding, JSON schema, WebSocket type,
   command и payload size.
5. Ошибки workers агрегируются; отказ всех workers завершает фазу раньше.
6. Worker имеет не более одного reconnect; throughput целиком не повторяется.
7. `duration` — 3–60 секунд, connections — 1–16, auto — не более 16.
8. Cancellation немедленно закрывает network I/O; Ctrl+C завершается примерно
   за секунду без goroutine leaks.
9. Ошибка не превращается в успешные `0 Мбит/с`.
10. Ошибка одного сервиса не останавливает `all`.

Не корректируй результат искусственным коэффициентом или clamp. Исправляй
выбор сервера, границы времени, byte accounting или protocol validation.

## Протоколы и данные подключения

Общие правила:

- используй только публичные first-party endpoints и browser credentials;
- не обходи CAPTCHA, browser challenge, rate limit или access control;
- не добавляй telemetry/perf reporting и отправку результатов;
- JWT, browser keys, IP-ответы и runtime credentials храни только в памяти;
- не печатай credentials и полные чувствительные ответы;
- discovery/ping разрешено повторить не более двух раз;
- protocol changes сверяй с актуальным official frontend и фиксируй источник;
- не используй third-party proxy как fallback.

Yandex:

- discovery — `get-probes` со строгой schema и URL validation;
- четыре последовательных latency request к каждому CDN, CDN — параллельно;
- основной ping — минимальный валидный RTT;
- download streams распределяются между ближайшими CDN;
- upload сначала WebSocket; считаются только `{"k":"u","b":...}` ack;
- HTTP `postUrl` — fallback, bytes засчитываются после успешного status;
- IP извлекается только из ограниченного bootstrap JSON страницы; ISP не
  извлекается.

speedtest.ru:

- discovery — `/api/nearest_servers`, выбор минимальной median latency;
- JWT — `/api/server/gentoken`, одно обновление после 401/403;
- browser key после 401/403 извлекается из same-origin page bundle один раз;
- download — `download.php?ckSize=...`, upload — `upload.php`, header `jwt`;
- 10 ping samples, нативный jitter, adaptive chunks, upload blocks 2 MiB;
- встроенный список серверов поддерживает только ping;
- connection info: `/api/asn_provider/ip`, затем `/api/asn_provider/asn` без
  JWT; IP обязателен, ISP необязателен.

`puls ip` использует speedtest.ru → Yandex. Явный сервис не получает fallback.
`all --show-ip` делает один такой lookup. Ошибка connection info при обычном
измерении не меняет aggregate status или exit code.

## Ошибки и JSON

Используй `ServiceID` (`service.ServiceID`), `Phase`, `Status`, `ErrorCode` и
`*service.OpError`. Ошибка содержит service, phase, code, retryable и cause.
Классифицируй через typed/sentinel errors, `errors.Is/As`, `context` и
`net.Error`; не анализируй текст сообщения.

JSON schema 1 — единый envelope:

```json
{
  "schema_version": 1,
  "command": "measure",
  "status": "ok",
  "connection": null,
  "results": []
}
```

Measurement result содержит `service`, `status`, `server`, `phases`, `error`,
`warnings`. Connection находится только наверху. Отсутствующие значения —
`null`, массивы — `[]`. Status: `ok|partial|error|canceled`, phase также
`skipped`. Exit codes: `0`, `1`, `2`, `130`. Ошибка записи JSON — code 1.

Изменение CLI/JSON требует синхронного обновления help, README, CHANGELOG и
golden tests.

## Порядок работы

1. Прочитай инструкции и `git status --short`.
2. Найди код и связанные тесты через `rg`.
3. Сначала определи проверяемый invariant или regression test.
4. Не перезаписывай несвязанные пользовательские изменения.
5. Форматируй Go через `gofmt`.
6. Сначала запускай targeted tests, затем полную проверку.
7. Перечитай diff и проверь отсутствие secrets/generated artifacts.

Не используй destructive Git commands. Не коммить `puls`, `dist/`, tokens,
production API responses или captures с персональными данными.

## Проверки

Перед релизом обязательны:

```sh
go test ./...
go test -count=10 ./...
go test -race ./...
go vet ./...
staticcheck ./...
govulncheck ./...
actionlint .github/workflows/*.yml
shellcheck scripts/install.sh
```

Network tests используют local HTTP/WebSocket mocks и покрывают success, exact
bytes, malformed frames/JSON, 401/403/5xx, disconnect, partial success,
reconnect, deadline и cancellation. Live tests находятся за tag `live`;
throughput запускается только с `PULS_LIVE_THROUGHPUT=1`.

Системные dev-зависимости на ALT Workstation устанавливай в Distrobox. Для
PowerShell используй контейнер `puls-powershell-dev`, описанный в
`docs/distribution.md`; Windows integration test остаётся обязательным в CI.

Release builder обязан сохранять шесть targets, CGO=0, reproducible archives,
manifest, SHA-256, installers, PATH/update/uninstall и ASCII without BOM для
`install.ps1`.

## Definition of Done

- поведение соответствует задаче и протоколам;
- mock/golden/regression tests обновлены;
- test ×10, race, vet и static/security checks прошли;
- help/docs совпадают с CLI и JSON;
- installers и шесть release targets проверены;
- diff не содержит secrets и случайных artifacts;
- live IP/ping и разрешённый acceptance throughput выполнены отдельно.
