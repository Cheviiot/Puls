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
distrobox enter puls-fyne-dev -- bash -lc \
  'cd "$PWD" && go run ./cmd/release --mode gui --targets linux/amd64 --version dev'
```

CLI-only сборка не требует CGO:

```sh
go build -tags nogui ./cmd/puls
```

## Артефакты

Release workflow собирает native GUI+CLI архивы для Linux, Windows amd64 и
macOS. Windows arm64 временно получает CLI-only архив. Android публикуется как
подписанный universal APK с application ID `io.github.cheviiot.puls`.

`RELEASE_MANIFEST.json` schema 2 указывает OS, architecture, kind,
capabilities и SHA-256 каждого пакета. `SHA256SUMS.txt` включает архивы, APK,
manifest и установщики. Архивы содержат binary, иконки, README, CHANGELOG и
LICENSE.

Установщики работают без прав администратора:

- Linux/macOS: `$HOME/.local/bin/puls` и ярлык приложения;
- Windows: `%LOCALAPPDATA%\Programs\Puls\bin` и ярлык Start Menu;
- `--no-shortcut` / `-NoShortcut` отключает ярлык;
- повторный запуск обновляет Puls;
- `--uninstall` / `-Uninstall` удаляет binary и управляемый ярлык.

PowerShell-скрипт обязан оставаться ASCII without BOM для Windows PowerShell 5.

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

## Выпуск v0.3.x

1. Все проверки `main` должны пройти.
2. Обновить CHANGELOG и `cmd/puls/FyneApp.toml`.
3. Создать неизменяемый тег соответствующей версии, например `v0.3.3`.
4. Workflow собирает пять GUI-архивов, Windows arm64 CLI и подписанный APK.
5. После checksum и provenance attestation draft публикуется автоматически.

Не перемещайте опубликованный тег и не заменяйте release assets.
