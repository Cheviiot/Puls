# Участие в разработке Puls

Спасибо за интерес к проекту. Пользовательский интерфейс Puls ориентирован на русскоязычную аудиторию: описания, результаты и ошибки пишутся по-русски, а команды, флаги, значения параметров, код и технические идентификаторы остаются на английском.

Перед первым изменением ознакомьтесь с [архитектурой](../docs/architecture.md) и
[кодексом поведения](CODE_OF_CONDUCT.md). Обязательные технические правила для
людей и AI-агентов закреплены в [AGENTS.md](../AGENTS.md).

## Подготовка среды

Puls переносится с Go на C++20. Новая реализация CLI, GUI на Qt Quick,
протоколов и engine находится в `src/`; Android и release builder пока
собираются из Go.

### C++

Нужны CMake 3.25+, Ninja, компилятор C++20 (GCC 13+, Clang 18+, Apple Clang
из Xcode 16+ или MSVC 2022) и [vcpkg](https://github.com/microsoft/vcpkg).
Зависимости из `vcpkg.json` устанавливаются при первой конфигурации; сборка Qt
занимает заметное время, без GUI проект собирается с `-DPULS_BUILD_GUI=OFF`.
На Linux для Qt из vcpkg нужны системные библиотеки X11:

```bash
sudo apt-get install '^libxcb.*-dev' libx11-xcb-dev libglu1-mesa-dev libxrender-dev \
  libxi-dev libxkbcommon-dev libxkbcommon-x11-dev libegl1-mesa-dev autoconf \
  autoconf-archive automake libtool
```

На macOS Qt из vcpkg требует autotools:

```bash
brew install autoconf autoconf-archive automake libtool
```

```bash
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
export VCPKG_ROOT=~/vcpkg

cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

На Windows используйте preset `windows-debug` в Developer PowerShell для
Visual Studio. Пути исходников, которые Qt генерирует при сборке, превышают
MAX_PATH, если каталог vcpkg расположен глубоко; укажите короткий каталог
сборки зависимостей: `-DVCPKG_INSTALL_OPTIONS=--x-buildtrees-root=C:/vb`.

Вместо vcpkg можно взять системные Boost 1.83+, OpenSSL 3, Qt 6.4+ и
GoogleTest через preset `system-debug`:

```bash
sudo apt-get install cmake ninja-build g++ libboost-dev libboost-json-dev libssl-dev \
  libgtest-dev qt6-base-dev qt6-declarative-dev qml6-module-qtquick \
  qml6-module-qtquick-controls qml6-module-qtquick-layouts qml6-module-qtquick-window \
  qml6-module-qtquick-templates qml6-module-qtqml-workerscript qt6-qpa-plugins
cmake --preset system-debug
cmake --build --preset system-debug
ctest --preset system-debug
```

### Go

Нужна версия Go, указанная в `go.mod`.

```bash
git clone https://github.com/Cheviiot/Puls.git
cd Puls
go mod download
go test ./...
```

На ALT Workstation desktop-зависимости Fyne запускаются в Distrobox-контейнере
`puls-fyne-dev`; точные команды приведены в [distribution.md](../docs/distribution.md).

## Перед pull request

Для C++ (изменения QML проверяйте и по снимкам экрана:
`PULS_GUI_SCREENSHOTS=<каталог> ./build/debug/tests/puls_gui_tests`):

```bash
git ls-files '*.cpp' '*.hpp' | xargs clang-format-18 -i
ctest --preset debug --repeat until-fail:10
cmake --preset sanitize && cmake --build --preset sanitize && ctest --preset sanitize
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

Для Go:

```bash
gofmt -w ./cmd ./internal
go test ./...
go test -race ./...
go vet ./...
staticcheck ./...
govulncheck ./...
actionlint .github/workflows/*.yml
shellcheck scripts/install.sh
```

Изменение поведения должно сопровождаться тестом. Сетевые сценарии не должны попадать в обычные модульные тесты.

При изменении `scripts/install.ps1` дополнительно выполните его `-Help` в PowerShell.
На ALT Workstation для этого используется Distrobox-контейнер, указанный в
[документе о дистрибуции](../docs/distribution.md).

## Изменения сетевых протоколов

- Используйте только публично поставляемые официальные конечные точки и браузерные протоколы сервиса.
- Не обходите CAPTCHA, проверку браузера, ограничения доступа и другие механизмы защиты.
- Не добавляйте секреты, пользовательские токены и закрытые учётные данные.
- Проверяйте состояние HTTP, схему ответа, тип кадра и подтверждённый объём данных.
- Добавляйте локальный тест для успешного ответа, повреждённых данных, сетевого обрыва и частичного результата.
- Не отправляйте телеметрию и результаты измерений обратно сервису.

Сетевые тесты запускаются отдельно:

```bash
cmake --preset debug -DPULS_LIVE_TESTS=ON && cmake --build --preset debug
ctest --preset debug -R '^Live\.'
go test -tags=live ./internal/service/...
PULS_LIVE_THROUGHPUT=1 go test -tags=live -run Live ./internal/service/...
```

## Коммиты и pull request

Пишите короткие английские сообщения коммитов в повелительной форме. Один pull request должен решать одну задачу. В описании укажите способ проверки, изменения CLI/JSON, влияние на расход трафика и возможные риски. При изменении поведения синхронно обновите README, help, CHANGELOG и соответствующий документ из `docs/`.

Об уязвимостях сообщайте по инструкции из [SECURITY.md](SECURITY.md), а не через открытую задачу.
