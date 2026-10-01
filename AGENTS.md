# Инструкции для AI-разработчиков Puls

Файл действует на весь репозиторий. Вложенные инструкции могут уточнять, но не
ослаблять требования к корректности измерений, безопасности и приватности.

## Продукт и язык

Puls — кроссплатформенное GUI- и CLI-приложение для русскоязычных пользователей. Оно измеряет
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
environment variables, JSON fields, C++ symbols, module names, code comments и
commit messages остаются английскими. Не добавляй кириллические flags.

## Публичный CLI

```text
puls
puls yandex [options]
puls speedtest [options]
puls all [options]
puls ip [speedtest|yandex] [options]
puls gui
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
для отдельного `speedtest`. Удалённый ещё в версиях на Go flag `--ip` не
восстанавливать.

Без аргументов TTY получает выбор сервиса, pipe запускает Yandex. Human output
идёт в `stdout`, verbose — в `stderr`. JSON не содержит ANSI или progress.

## Архитектурные границы

Puls написан на C++20: CMake ≥ 3.25, vcpkg manifest, Boost.Asio/Beast/JSON,
OpenSSL 3, Qt 6 Quick и GoogleTest; Android-приложение собирается с Qt for
Android.

- `src/puls/cli`: parsing, configuration, orchestration, results, JSON,
  rendering, запуск `puls-gui`;
- `src/puls/application`: единая orchestration и модели для CLI/GUI
  (`app::Runner`);
- `src/puls/gui`: `model` — состояние и поведение dashboard без Qt;
  `controller`, `run` и `qml/` — тонкий слой Qt Quick над моделью, тема,
  lifecycle и безопасные preferences; `main.cpp` — программа `puls-gui`;
- `src/puls/measure`: общий concurrency engine;
- `src/puls/service`: `Backend`, `ConnectionInfoBackend`, общие types/helpers;
- `src/puls/service/yandex`: только протокол Яндекса;
- `src/puls/service/speedtestru`: только протокол speedtest.ru;
- `src/puls/ui`: TTY, menu, colors, progress;
- `src/puls/core`: `Context`/`CancelScope`, `Error`/`Result`, прерывания
  (Ctrl+C, SIGTERM), JSON, IP, text;
- `src/puls/net`: HTTP/1.1, WebSocket и TLS с системными корнями;
- `tests`: GoogleTest, `tests/support` — локальные HTTPS/WSS mocks;
- `cmake`: общие опции, упаковка, overlay triplets и ports vcpkg, toolchain
  Android;
- `scripts`: direct installers; `scripts/release.py` — архивы выпуска,
  manifest и checksums; `scripts/tests` — тесты инструмента выпуска и
  установщиков;
- `docs`: архитектура и выпуск.

Передавай logger и внешние зависимости через constructors/options. Не добавляй
global output, setter вида `set_verbose` или UI в `MeasurementConfig`. Общие
лимиты и преобразования принадлежат `src/puls/service`; правила конкретного
протокола не выноси туда.

Правила C++:

- отмена идёт только через `Context`; блокирующая операция после `ctx.done()`
  закрывает сокет и возвращается, а не ждёт timeout;
- ошибки — `Error` с тегами и деталями (`is`/`as` вместо `errors.Is/As`),
  значения — `Result<T>`; сетевые и протокольные ошибки не передаются
  исключениями, текст сообщения не анализируется;
- потоки workers всегда join-ятся; detached-поток допустим только для
  отменяемого DNS resolve;
- зависимости добавляются только в `vcpkg.json`; `builtin-baseline` и
  `VCPKG_COMMIT` в `.github/workflows/cpp.yml` меняются вместе; baseline
  обновляется при исправлениях безопасности OpenSSL, Boost и Qt;
- код форматируется `clang-format` 18 по `.clang-format`; предупреждения
  компилятора в CI считаются ошибками.

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
   за секунду, все потоки workers завершены.
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

Используй `service::ServiceId`, `Phase`, `Status`, `ErrorCode` и
`service::OpError`. Ошибка содержит service, phase, code, retryable и cause.
Классифицируй через теги и детали `Error` (`is`/`as`) и отмену `Context`; не
анализируй текст сообщения.

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

## GUI

- Qt 6 Quick/QML со стилем Material. Логику и тексты держи в
  `puls::gui::Dashboard`, QML только отображает свойства и вызывает методы
  `DashboardController`. События runner приходят из рабочего потока и
  передаются в UI-поток только queued-вызовом; Qt-типы между потоками не
  передаются.
- CLI и GUI обязаны вызывать `app::Runner`; не дублируй orchestration, retry
  или преобразование результатов во frontend.
- Сохраняй только тему, сервис, профиль, duration, connections, phase и размер
  окна. Не сохраняй IP, ISP, server, результаты, warnings, logs и credentials.
- Держи один mobile-first dashboard без gauges и истории: cyan accent,
  системная/светлая/тёмная темы, понятные состояния start/stop/error.
- Background/close отменяет активное измерение через context.
- `puls` без аргументов остаётся CLI и не зависит от Qt. Окно открывает
  отдельная программа `puls-gui`; `puls gui` запускает её из каталога `puls`
  (на macOS также `Puls.app`). Android всегда запускает GUI.

## Порядок работы

1. Прочитай инструкции и `git status --short`.
2. Найди код и связанные тесты через `rg`.
3. Сначала определи проверяемый invariant или regression test.
4. Не перезаписывай несвязанные пользовательские изменения.
5. Форматируй C++ через `clang-format` 18.
6. Сначала запускай targeted tests, затем полную проверку.
7. Перечитай diff и проверь отсутствие secrets/generated artifacts.

Не используй destructive Git commands. Не коммить `puls`, `dist/`, `build/`,
tokens, production API responses или captures с персональными данными.

## Проверки

Перед релизом обязательны:

```sh
cmake --preset debug && cmake --build --preset debug
ctest --preset debug --repeat until-fail:10
cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
git ls-files '*.cpp' '*.hpp' | xargs clang-format-18 --dry-run --Werror
python3 -m unittest discover -s scripts/tests
actionlint .github/workflows/*.yml
shellcheck scripts/install.sh .github/scripts/*.sh
```

Без vcpkg используй preset `system-debug` с системными Boost ≥ 1.83,
OpenSSL ≥ 3.0, Qt ≥ 6.4 и GTest; санитайзеры включает `-DPULS_SANITIZERS=...`,
сборку без Qt — `-DPULS_BUILD_GUI=OFF`. GUI-тесты работают с
`QT_QPA_PLATFORM=offscreen`; `PULS_GUI_SCREENSHOTS=<dir>` сохраняет снимки
экрана для проверки вёрстки. Live tests собираются с `-DPULS_LIVE_TESTS=ON`.
Workflow `cpp.yml` обязан проходить для шести desktop targets (Linux, macOS
и Windows на x64 и ARM64; Windows ARM64 — только CLI), Android, Linux Clang,
ASan+UBSan и TSan. Зависимости из vcpkg, включая Qt, линкуются статически,
на Windows и CRT (overlay triplets в `cmake/triplets`), поэтому программам не
нужны DLL; на Linux и macOS зависимости собираются только в Release. Linux с GUI
собирается в контейнере manylinux_2_28, чтобы программам хватало glibc 2.28.
TSan собирается без Qt: неинструментированный Qt синхронизирует очередь
событий через futex, и TSan даёт ложные срабатывания. По той же причине
`tests/tsan.supp` подавляет чтения OpenSSL из его хеш-таблиц под RCU.

Network tests используют local HTTP/WebSocket mocks и покрывают success, exact
bytes, malformed frames/JSON, 401/403/5xx, disconnect, partial success,
reconnect, deadline и cancellation. Live tests собираются только с
`-DPULS_LIVE_TESTS=ON`; throughput запускается только с
`PULS_LIVE_THROUGHPUT=1`.

Системные dev-зависимости на ALT Workstation устанавливай в Distrobox. C++ с
системными библиотеками собирай и тестируй в `puls-dev`, PowerShell — в
`puls-powershell-dev`; команды описаны в `docs/distribution.md`. Windows
integration test установщика обязателен в CI.

Выпуск обязан сохранять шесть desktop targets: `puls` и `puls-gui` для Linux,
macOS и Windows на x64 и ARM64 и только CLI для Windows ARM64. Сохраняй
reproducible archives, manifest schema 3, SHA-256, installers,
PATH/update/uninstall, управляемые shortcuts и ASCII without BOM для
`install.ps1`. Изменения архивов, manifest и установщиков проверяй
`python3 -m unittest discover -s scripts/tests`. Android APK
подписывается только секретами GitHub Actions; keystore не добавлять в Git.

Версию повышай на один шаг: исправления — patch, новые возможности — minor;
1.0.0 — только по решению владельца. Нумерация начата заново с 0.1.0 после
удалённых выпусков 0.1.0–0.3.3 на Go, поэтому version code Android содержит
смещение 10000 (`src/CMakeLists.txt`); не убирай его.

## Definition of Done

- поведение соответствует задаче и протоколам;
- mock/golden/regression tests обновлены;
- ctest ×10, ASan+UBSan, TSan, clang-format, actionlint, shellcheck и тесты
  выпуска прошли;
- help/docs совпадают с CLI и JSON;
- installers, шесть desktop targets и APK проверены;
- diff не содержит secrets и случайных artifacts;
- live IP/ping и разрешённый acceptance throughput выполнены отдельно.
