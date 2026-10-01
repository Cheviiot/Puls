# Сборка и выпуск

## Среда разработки на ALT

Системные зависимости устанавливаются не на host, а в Distrobox. Контейнер
`puls-dev` собирает C++ версию с библиотеками Ubuntu 24.04 через preset
`system-debug`:

```sh
distrobox create --name puls-dev --image docker.io/library/ubuntu:24.04 --yes
distrobox enter puls-dev -- sudo apt-get update
distrobox enter puls-dev -- sudo apt-get install -y \
  cmake ninja-build g++ git python3 shellcheck clang-format-18 \
  libboost-dev libboost-json-dev libssl-dev libgtest-dev \
  qt6-base-dev qt6-declarative-dev qml6-module-qtquick qml6-module-qtquick-controls \
  qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-templates \
  qml6-module-qtqml-workerscript qt6-qpa-plugins
```

Сборка и проверка:

```sh
distrobox enter puls-dev -- bash -lc 'cd "$PWD" &&
  cmake --preset system-debug && cmake --build --preset system-debug &&
  QT_QPA_PLATFORM=offscreen ctest --preset system-debug &&
  python3 -m unittest discover -s scripts/tests'
```

`install.ps1` проверяется в контейнере `puls-powershell-dev`:

```sh
distrobox create --name puls-powershell-dev --image mcr.microsoft.com/powershell:latest --yes
distrobox enter puls-powershell-dev -- bash -lc 'cd "$PWD" && pwsh -NoProfile -File scripts/install.ps1 -Help'
```

## Артефакты

Release workflow вызывает workflow `C++`: он собирает, тестирует и упаковывает
шесть desktop-архивов — Linux, macOS и Windows на x64 и ARM64; для Windows
ARM64 только CLI. Android публикуется как подписанный universal APK с
application ID `io.github.cheviiot.puls`.

Архив содержит CLI `puls` и графическое приложение `puls-gui` (на macOS —
`Puls.app`), README, CHANGELOG, LICENSE и `THIRD_PARTY_NOTICES.txt` с
лицензиями зависимостей; для Linux — ещё значок для ярлыка. Программы
слинкованы статически и не требуют установки библиотек, кроме системных
библиотек X11 на Linux.

`scripts/release.py package` превращает каталог `cmake --install` с
`PULS_PORTABLE_INSTALL=ON` в воспроизводимый архив: записи отсортированы,
время, владельцы и права фиксированы. `scripts/release.py assemble` проверяет
архивы всех шести целей и APK и записывает `RELEASE_MANIFEST.json` schema 3
(OS, architecture, kind, capabilities и SHA-256 каждого пакета) и
`SHA256SUMS.txt`, включая manifest и установщики.

Установщики работают без прав администратора:

- Linux/macOS: `$HOME/.local/bin/puls` и `puls-gui` (Linux) с ярлыком в меню
  приложений или `~/Applications/Puls.app` (macOS);
- Windows: `%LOCALAPPDATA%\Programs\Puls\bin\puls.exe` и `puls-gui.exe` с
  ярлыком Start Menu;
- `--no-shortcut` / `-NoShortcut` отключает ярлык; на macOS — установку
  `Puls.app`;
- на Linux установщик называет библиотеки X11, которых не хватает
  `puls-gui` в системе (чаще всего `libxcb-cursor0`);
- повторный запуск обновляет Puls;
- `--uninstall` / `-Uninstall` удаляет программы и управляемый ярлык.

Установщики принимают только manifest schema 3; для выпусков до 0.4.0
используйте установщик соответствующего выпуска. PowerShell-скрипт обязан
оставаться ASCII without BOM для Windows PowerShell 5.

## Android

Workflow `C++` собирает APK из той же C++ версии: Qt for Android 6.11.2
(aqtinstall сверяет SHA-256 архивов с репозиторием Qt), Android NDK r27c,
SDK platform 36, build-tools 36.0.0 и JDK 17. Один APK содержит библиотеки
для arm64-v8a, armeabi-v7a и x86_64 и требует Android 9 (API 28). Qt собирает
каждый ABI отдельным проектом; `cmake/android-toolchain.cmake` выбирает для
него triplet vcpkg из `cmake/triplets`, и vcpkg собирает Boost и OpenSSL.

Workflow проверяет имя пакета, ABI, разрешения (приложению нужен только
доступ в сеть) и выравнивание библиотек по 16 КБ.
`src/puls/gui/android/AndroidManifest.xml` — шаблон Qt 6.11.2 без разрешения
на запись в память, которое Qt Core запрашивает по умолчанию. Библиотеки в APK
сжаты: его скачивают напрямую, а не из магазина приложений. При выпуске APK
выравнивается `zipalign`, подписывается `apksigner` и проверяется. Ключ
подписи хранится в repository secrets:

```text
PULS_ANDROID_KEYSTORE_BASE64
PULS_ANDROID_KEYSTORE_PASSWORD
PULS_ANDROID_KEY_ALIAS
PULS_ANDROID_KEY_PASSWORD
```

Keystore не хранится в Git. Application ID `io.github.cheviiot.puls` и ключ
прежних выпусков не меняются, поэтому APK обновляет установленное
приложение. Код версии вычисляется из версии выпуска:
`major * 1000000 + minor * 1000 + patch`.

## Выпуск

1. Все проверки `main` должны пройти.
2. Добавить раздел версии в CHANGELOG.
3. Запустить release workflow вручную с версией: preflight собирает и
   проверяет все артефакты без публикации.
4. Создать неизменяемый тег версии, например `v0.4.0`.
5. Workflow собирает шесть desktop-архивов и подписанный APK; после checksum
   и provenance attestation draft публикуется автоматически.

Не перемещайте опубликованный тег и не заменяйте release assets.
