# Puls C++ / Qt

Пользователь явно разрешил перенос на C++ и потребовал новый GUI с нуля.
Текущий интерфейс использует Qt Widgets; Go/Fyne остаётся reference-реализацией.
Общие протоколы, CLI/JSON, приватность и ограничения измерений из корневого
`AGENTS.md` обязательны и здесь.

## Границы

- `include/puls/core.hpp`, `src/core.cpp`: модели, typed `OpError`, один `Runner`.
- `src/backend.cpp`: discovery, ping и данные подключения двух сервисов.
- `src/transfer.cpp`: readiness, warm-up, deadline, concurrency и confirmed bytes.
- `src/yandex_throughput.cpp`, `src/speedtest_throughput.cpp`: протоколы передачи.
- `src/cli.cpp`, `src/main.cpp`: parsing, stdout/stderr, TTY и exit codes.
- `src/gui.cpp`: новый адаптивный экран, настройки и Qt lifecycle.
- `android/`: Android manifest, TLS runtime, проверка APK и эмулятор.
- `../cmake/`, `../scripts/`: native deployment, версии, архивы и установщики.

CLI и GUI вызывают общий `Runner`. Qt network objects создаются, используются
и уничтожаются в вызывающем потоке; GUI запускает Runner в `QThread` и получает
копии событий через очередь. Не обращайся к widgets из worker threads.
Передавай зависимости через constructors/options; output и UI не принадлежат
сетевым backends. Cancellation закрывает I/O и завершает workers до возврата.

## GUI

Используй Qt Widgets, нейтральные поверхности, синий accent и отдельную страницу
параметров. GTK runtime не нужен. Сохраняй только разрешённые настройки;
IP, ISP, результаты, серверы, logs и credentials остаются в памяти.
Главный экран адаптируется к 360×294, 360×640, 900×600 и 1280×800 в обе стороны.
Остановка доступна при прокрутке; сначала выделяется ширина, затем считается
высота текста. Не уменьшай шрифт и не скрывай результаты ради размера.

## Проверки и выпуск

Системные зависимости устанавливаются в Distrobox; локальная среда описана в
`../docs/cpp-migration.md`. Релизный SDK — Qt 6.12 или новее; Linux amd64 SDK
собирается на Ubuntu 22.04 для совместимости с ALT. Fedora runtime нельзя
объявлять переносимым для host с более старой glibc.

Начинай с targeted CTest, затем полный CTest ×10, ASan/UBSan, cppcheck и нужные
проверки concurrency. Network tests используют локальные mocks. Live throughput
требует `PULS_LIVE_THROUGHPUT=1`; credentials и production-ответы не сохраняются.
Изменения CLI/JSON синхронизируются с help, README, CHANGELOG и golden tests.

Проверяй шесть native desktop targets, relocation/SSL/GUI, manifest schema 2,
SHA-256 и установку/обновление/удаление. Windows arm64 остаётся `nogui`.
Android APK подписывается только существующими GitHub Actions secrets; затем
проверяются обе ABI, 16 KiB alignment и запуск в эмуляторе.
