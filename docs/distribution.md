# Сборка и выпуск

## Среда разработки на ALT

Fyne-зависимости устанавливаются не на host, а в Distrobox:

```sh
distrobox create --name puls-fyne-dev --image docker.io/library/ubuntu:24.04 --yes
distrobox enter puls-fyne-dev -- sudo apt-get update
distrobox enter puls-fyne-dev -- sudo apt-get install -y \
  golang-go git gcc pkg-config libgl1-mesa-dev xorg-dev libwayland-dev libxkbcommon-dev
```

Проверка и native Linux build:

```sh
distrobox enter puls-fyne-dev -- bash -lc 'cd "$PWD" && go test ./...'
distrobox enter puls-fyne-dev -- bash -lc 'cd "$PWD" && go build ./cmd/puls'
```

CLI-only сборка не требует CGO:

```sh
go build -tags nogui ./cmd/puls
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
- повторный запуск обновляет Puls;
- `--uninstall` / `-Uninstall` удаляет программы и управляемый ярлык.

Установщики принимают только manifest schema 3; для выпусков до 0.4.0
используйте установщик соответствующего выпуска. PowerShell-скрипт обязан
оставаться ASCII without BOM для Windows PowerShell 5.

## Android signing

Repository secrets:

```text
PULS_ANDROID_KEYSTORE_BASE64
PULS_ANDROID_KEYSTORE_PASSWORD
PULS_ANDROID_KEY_ALIAS
PULS_ANDROID_KEY_PASSWORD
```

Keystore не хранится в Git. Workflow проверяет подпись APK и отклоняет
неожиданные чувствительные permissions; приложению нужен только доступ в сеть.
Fyne сначала создаёт подписанный Android App Bundle, затем проверенный по
SHA-256 `bundletool` формирует из него подписанный universal APK для прямой
установки вне магазина приложений.

## Выпуск

1. Все проверки `main` должны пройти.
2. Добавить раздел версии в CHANGELOG и обновить `cmd/puls/FyneApp.toml` для
   Android.
3. Запустить release workflow вручную с версией: preflight собирает и
   проверяет все артефакты без публикации.
4. Создать неизменяемый тег версии, например `v0.4.0`.
5. Workflow собирает шесть desktop-архивов и подписанный APK; после checksum
   и provenance attestation draft публикуется автоматически.

Не перемещайте опубликованный тег и не заменяйте release assets.
