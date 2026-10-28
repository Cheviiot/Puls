# Участие в разработке Puls

Puls — приложение для русскоязычных пользователей. Пользовательские сообщения,
описания, результаты и ошибки пишутся по-русски; команды, флаги, значения
параметров, код и технические идентификаторы остаются на английском.

Перед изменением ознакомьтесь с [архитектурой](../docs/architecture.md),
[правилами проекта](../AGENTS.md), [правилами C++](../cpp/AGENTS.md) и
[руководством по переносу](../docs/cpp-migration.md) и
[кодексом поведения](CODE_OF_CONDUCT.md). Текущая основная
реализация — C++20 с CMake и Qt Widgets. Go-код в `cmd/` и `internal/`
сохраняется как reference-реализация опубликованных версий.

## Среда разработки

Нужны C++20, CMake 3.21 или новее и Qt с модулями Core, Network, WebSockets и
Widgets. На ALT Workstation используйте Distrobox; контейнер, зависимости и
команды описаны в [cpp-migration.md](../docs/cpp-migration.md). Для выпуска
используется Qt 6.12 или новее; Linux amd64 release SDK собирается на Ubuntu
22.04 для совместимости с glibc на ALT. Не объявляйте платформу проверенной
только потому, что для неё есть CMake-конфигурация.

Пример локальной сборки из каталога репозитория:

```bash
cmake -S . -B build/cpp-debug -DCMAKE_BUILD_TYPE=Debug -DPULS_BUILD_GUI=ON
cmake --build build/cpp-debug --parallel 2
ctest --test-dir build/cpp-debug --output-on-failure
```

Чтобы собрать CLI без GUI, задайте `-DPULS_BUILD_GUI=OFF`. Для конкретного
контракта используйте `ctest --test-dir build/cpp-debug -R '<test-regex>'`.

## Проверки перед pull request

Запускайте подходящие targeted CTest для изменения, затем полный набор CTest
десять раз:

```bash
ctest --test-dir build/cpp-debug --output-on-failure --repeat until-fail:10
```

При изменении engine или сетевого кода запускайте ASan/UBSan и, если затронута
конкурентность, TSan для engine там, где он поддерживается. Текущий sanitizer
CI запускает ASan/UBSan; CMake-флаг `PULS_ENABLE_SANITIZERS=ON` включает эти два
санитайзера. Выполните статический анализ C++:

```bash
cmake -S . -B build/asan -DCMAKE_BUILD_TYPE=Debug \
  -DPULS_BUILD_GUI=OFF -DPULS_ENABLE_SANITIZERS=ON
cmake --build build/asan --parallel 2
ctest --test-dir build/asan --output-on-failure
cppcheck --enable=warning,performance,portability --error-exitcode=1 \
  --std=c++20 --library=qt --suppress=missingIncludeSystem --inline-suppr \
  -Icpp/include cpp/src cpp/tests
```

Проверки упаковки, APK, workflow и установщиков:

```bash
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/tests -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s cpp/android/ci -p 'test_*.py' -v
actionlint .github/workflows/*.yml
shellcheck scripts/install.sh
pwsh -NoProfile -File ./scripts/install.ps1 -Help
```

CI дополнительно проверяет Windows runtime installer и запускает нативную
матрицу: Linux amd64/arm64, Windows amd64, macOS amd64/arm64 с GUI и CLI, а
также Windows arm64 в режиме CLI-only. Android workflow проверяет и запускает
подписанный multi-ABI APK в эмуляторе. Отмечайте платформенную проверку как
успешную только после результата соответствующего CI job.

Изменяя сетевой протокол, используйте публичные first-party endpoints и
проверяйте HTTP status, schema, frame type и только подтверждённые bytes. Не
обходите CAPTCHA, browser challenge, rate limits или access control; добавляйте
локальные mock-тесты для ответов и сетевых ошибок.

Публичные команды, flags, выходные коды и JSON schema 1 должны сохранять
совместимость. При их изменении обновляйте help, README, CHANGELOG и golden
проверки. Сетевые тесты используйте с локальными mocks; live throughput
запускается только при явном `PULS_LIVE_THROUGHPUT=1`.

Не добавляйте telemetry или отправку результатов. IP, ISP, серверы измерения,
результаты и credentials остаются в памяти. Не коммитьте tokens, keystore или
производственные ответы; APK подписывается только существующими GitHub Actions
secrets.

### Go reference-проверки

Если pull request меняет Go reference-код в `cmd/` или `internal/`, отдельно
запустите соответствующие legacy checks:

```bash
go test ./...
go test -race ./...
go vet ./...
staticcheck ./...
govulncheck ./...
```

Они не заменяют проверки C++/Qt выше.

## Коммиты и pull request

Пишите короткие английские сообщения коммитов в повелительной форме. Один pull
request должен решать одну задачу. Укажите изменение, способ проверки,
совместимость CLI/JSON, влияние на сетевой трафик и возможные риски. При
изменении поведения синхронно обновите README, help, CHANGELOG и относящийся к
изменению документ из `docs/`.

Об уязвимостях сообщайте по инструкции из [SECURITY.md](SECURITY.md), а не
через открытую задачу.
