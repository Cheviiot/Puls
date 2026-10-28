# Перенос Puls на C++

Рабочая ветка: `codex/cpp-migration`. Пользователь запросил ускоренный перенос
26 октября 2026 года. Перенос ведётся рядом с Go-реализацией: опубликованные
версии используют проверенную Go-сборку. Новый release workflow уже вызывает
C++ desktop и Qt Android сборки; публикация новой версии требует полного preflight.

## Принятые решения

- C++20, CMake 3.21+, Qt 6.5+ с Core, Network, WebSockets и Widgets.
- Релизный SDK — Qt 6.12.0 LTS. Linux amd64 Qt собирается из проверенных
  исходников на Ubuntu 22.04 для совместимости с glibc 2.38 на ALT; SDK из
  Fedora-контейнера используется только для разработки внутри контейнера.
  Linux-пакеты используют системные fontconfig и FreeType: старая библиотека
  из архива может не понимать настройки шрифтов более нового дистрибутива.
- Одно ядро `puls_core` и один `Runner` для CLI и GUI. Сетевые объекты создаются
  и уничтожаются в потоке вызова; GUI получает копии событий через очередь Qt.
- Qt заменяет Fyne в новой C++-реализации по прямому запросу пользователя о
  полном переносе. По уточнению «новый GUI с нуля» интерфейс проектируется
  заново: выбор сервиса на главном экране, адаптивные карточки показателей,
  отдельные настройки и закреплённая кнопка запуска/остановки. GTK runtime
  не требуется.
- Целевые платформы: Linux amd64/arm64, Windows amd64/arm64, macOS amd64/arm64,
  Android. Наличие CMake-конфигурации не означает проверенный выпуск на платформе.
- Публичные команды, ограничения flags, русский human output и JSON schema 1
  сохраняются. CLI без аргументов остаётся CLI; Android открывает GUI.
- JWT, browser keys, IP, ISP и результаты не записываются в preferences.

## Текущее состояние

Реализованы модели результатов, ошибки, Runner, CLI, новый Qt GUI, discovery,
задержка и данные подключения двух сервисов. Download/upload используют общий
конкурентный C++ engine. Опубликованные стабильные версии собраны из Go;
переносимая упаковка новой версии проверена на всех целевых платформах.

Полный preflight 0.4.0 прошёл в [GitHub Actions](https://github.com/Cheviiot/Puls/actions/runs/40687420798):
шесть native desktop архивов, подписанный multi-ABI Android APK, общий manifest
schema 2, SHA-256 и установщики. Windows installer проверяет установку,
обновление, откат и удаление. Linux amd64 runtime дополнительно запущен прямо
на ALT без контейнера; CLI/JSON, SSL и 42 GUI-снимка прошли. Android эмулятор
подтвердил foreground, background/resume и работающий OpenSSL backend без
публичного измерения. GitHub Release и тег не публиковались.

Движок начинает таймер после готовности потока и разогрева. Он учитывает только
подтверждённые байты внутри окна измерения, завершает фазу при отказе всех
потоков и допускает одно переподключение каждого потока. Закрытие сетевых
объектов исключено из измеренного времени. `--connections=0` включает
адаптивный режим до 16 потоков, явное число фиксирует количество потоков.

Яндекс скачивает проверенные бинарные HTTP-ответы; отдача считает только
WebSocket ACK `{"k":"u","b":...}` либо полностью успешные ответы HTTP
`postUrl`. speedtest.ru использует JWT, одно обновление токена после 401/403,
ограниченное обновление browser key, адаптивные download chunks и upload
блоки 2 MiB. Credentials остаются в памяти. GUI получает текущую скорость
через события того же Runner. При ошибке скорость — `null`, подтверждённые
байты, время, счётчики потоков и warnings остаются в диагностике schema 1.

Go-код и существующие незакоммиченные изменения Fyne сохранены. До переключения
релизов необходимы проверки всех целевых платформ, оставшийся паритет поведения,
выпуск и установщики. Не допускаются искусственные коэффициенты результатов.

Встроенный резервный список speedtest.ru используется при отказе discovery
только для ping; download/upload в этом режиме возвращают явную ошибку без
запроса JWT. Для Яндекса 26.10.2026 проверены актуальные
`get-probes` и [официальный frontend r21702543](https://yastatic.net/s3/internetometer/r21702543/desktop.bundle.ru.js):
`websocketConnectionTimeout` — необязательное переопределение. При отсутствии
этого поля discovery принимается без выдуманного timeout; явно заданное значение
проверяется как целое 1–30000. Для upload официальный frontend задаёт default
connection timeout 15000 мс, binary frame 64 KiB и предел очереди отправки
1 MiB. Общий startup deadline engine дополнительно ограничивает ожидание.
Production-ответы и credentials не сохранялись.

Обычный TTY показывает текущую фазу и скорость, затем очищает строку перед
итогом. `--verbose` выводит ограниченную диагностику в stderr. Pipe и JSON
не получают прогресс и ANSI; `--no-color`, `NO_COLOR` и `TERM=dumb` отключают
цвет TTY. JSON schema 1 остаётся прежней.

Нативные CI Windows, Linux и macOS проверяют девять групп CTest с повтором ×10;
отдельно запускаются ASan/UBSan и cppcheck. Локально проверен TSan общего engine.
Короткий live-прогон обоих сервисов (`all --duration 3 --connections 1`) прошёл
все фазы при явно включённом `PULS_LIVE_THROUGHPUT=1`. Прерывание живой отдачи
Ctrl+C вернуло JSON `canceled` и code 130 примерно за 136 мс. Это проверки
работоспособности протокола; сравнение точности с браузером проводится отдельно.

## Сборка

На ALT зависимости находятся в Distrobox. Для первого этапа используется
существующий Fedora-контейнер `aeonlc-build` с CMake/GCC. Установлены только
дополнительные Qt-пакеты, без обновления toolchain:

```sh
podman exec --user 0 aeonlc-build sh -lc \
  'ionice -c 3 nice -n 19 dnf install -y --setopt=max_parallel_downloads=2 qt6-qtbase-devel qt6-qtwebsockets-devel'
podman exec --user 1000 --workdir "$PWD" aeonlc-build \
  sh -lc 'ionice -c 3 nice -n 19 cmake -S . -B build/cpp-debug -DCMAKE_BUILD_TYPE=Debug'
podman exec --user 1000 --workdir "$PWD" aeonlc-build \
  sh -lc 'ionice -c 3 nice -n 19 cmake --build build/cpp-debug --parallel 2'
podman exec --user 1000 --workdir "$PWD" aeonlc-build \
  sh -lc 'ctest --test-dir build/cpp-debug --output-on-failure'
```

На системе с установленным Qt достаточно тех же трёх команд CMake/CTest без
`podman exec`. Для CLI-only используйте `-DPULS_BUILD_GUI=OFF`. Windows
собирается MSVC с UTF-8, macOS — Clang. Android требует Qt Android kit,
официальный SDK/NDK и Qt 6.12 toolchain; точные версии и проверка APK описаны
в [Android-инструкции](../cpp/android/README.md). APK подписывается существующим
ключом только в CI. Для sanitizers в Fedora нужны `libasan`, `libubsan` и `libtsan`.

На icebook дополнительно установлен официальный Qt 6.12.0 SDK в
`~/.local/share/toolchains/puls-qt612-dev/6.12.0/gcc_64`. Он используется только
внутри `aeonlc-build`: его готовые библиотеки требуют glibc 2.39 и не заменяют
переносимый SDK, собранный на Ubuntu 22.04. Системный Qt контейнера не меняется.
Точная локальная сборка и запуск:

```sh
podman exec --user 1000 --workdir "$PWD" aeonlc-build sh -lc \
  'ionice -c 3 nice -n 19 cmake -S . -B build/cpp-qt612 -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DCMAKE_PREFIX_PATH=/home/cheviiot/.local/share/toolchains/puls-qt612-dev/6.12.0/gcc_64 && ionice -c 3 nice -n 19 cmake --build build/cpp-qt612 --parallel 2'
distrobox enter aeonlc-build -- env QT_QPA_PLATFORMTHEME= "$PWD/build/cpp-qt612/puls" gui
```

Дополнительные инструменты проверок устанавливаются в тот же контейнер:

```sh
podman exec --user 0 aeonlc-build sh -lc \
  'ionice -c 3 nice -n 19 dnf install -y --setopt=max_parallel_downloads=2 libasan libubsan libtsan cppcheck'
```

Статический анализ выполняет `cppcheck` (в Fedora-контейнере устанавливается
пакет `cppcheck`); та же проверка включена в Linux CI:

```sh
cppcheck --enable=warning,performance,portability --error-exitcode=1 \
  --std=c++20 --library=qt --suppress=missingIncludeSystem --inline-suppr \
  -Icpp/include cpp/src cpp/tests
```

```sh
build/cpp-debug/puls help
build/cpp-debug/puls yandex --only ping --json
build/cpp-debug/puls ip
build/cpp-debug/puls gui
```

Live throughput не запускается без `PULS_LIVE_THROUGHPUT=1`. Тесты первого
переноса используют внедряемые backend и локальные mock-серверы; персональные
production-ответы не сохраняются.

## Условия переключения релиза

1. Перенести конкурентный engine: готовность worker, исключение warm-up,
   подтверждённые bytes, ограничение reconnect и раннее завершение при отказе.
2. Перенести Yandex WebSocket ack и HTTP fallback, speedtest JWT refresh,
   browser-key refresh, adaptive chunks и native concurrency.
3. Достичь соответствия CLI/JSON, добавить golden-проверки обеих реализаций.
4. Проверить GUI на 360×294, 360×640, 900×600 и 1280×800, темы, resize,
   start/stop/error, закрытие и уход в background.
5. Пройти mock-тесты, повтор ×10, sanitizers и статический анализ. Сохранить
   проверки Go до завершения переноса.
6. Собрать и проверить шесть desktop targets, Android APK signing в CI,
   manifest schema 2, checksums, установку/обновление/удаление и shortcuts.
7. Обновить основные README, CHANGELOG, архитектуру и инструкции одновременно
   с переключением стабильной реализации. Не объявлять этап завершённым без
   результатов проверок каждой платформы.
