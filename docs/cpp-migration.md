# Перенос Puls на C++

Рабочая ветка: `codex/cpp-migration`. Пользователь запросил ускоренный перенос
26 октября 2026 года. Перенос ведётся рядом с Go-реализацией: опубликованные
версии и установщики продолжают использовать проверенную Go-сборку до
достижения функционального соответствия.

## Принятые решения

- C++20, CMake 3.21+, Qt 6.5+ с Core, Network, WebSockets и Widgets.
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

## Первый этап

Переносятся модели результатов, ошибки, Runner, CLI, Qt GUI, discovery,
задержка и данные подключения двух сервисов. Download/upload до переноса
общего конкурентного engine возвращают явную ошибку `unavailable`; числовой
результат скорости остаётся `null`. Это рабочая версия для разработки,
не замена стабильного релиза.

Go-код и существующие незакоммиченные изменения Fyne сохранены. До переключения
релизов необходимы перенос engine и обоих throughput-протоколов, весь набор
HTTP/WebSocket mock-тестов, отмена активного I/O, byte accounting и сравнение
с текущей реализацией. Не допускаются искусственные коэффициенты результатов.

Встроенный резервный список speedtest.ru для ping ещё не перенесён; при отказе
discovery пока возвращается ошибка. Для Яндекса 26.10.2026 проверены актуальные
`get-probes` и [официальный frontend r21702543](https://yastatic.net/s3/internetometer/r21702543/desktop.bundle.ru.js):
`websocketConnectionTimeout` — необязательное переопределение. При отсутствии
этого поля discovery принимается без выдуманного timeout; явно заданное значение
проверяется как целое 1–30000. Production-ответы и credentials не сохранялись.

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
официальный SDK/NDK, Qt 6.8+ и соответствующий Qt toolchain; APK пока не
публикуется. Для sanitizers в Fedora нужны `libasan` и `libubsan`.

```sh
build/cpp-debug/puls help
build/cpp-debug/puls yandex --only ping --json
build/cpp-debug/puls ip
build/cpp-debug/puls gui
```

Live throughput не запускается без `PULS_LIVE_THROUGHPUT=1`. Тесты первого
этапа используют внедряемые backend и локальные mock-серверы; персональные
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
