# Дистрибуция Puls

Puls устанавливается напрямую из GitHub Releases. Проект не требует отдельного
Homebrew tap, Scoop bucket, package repository или зеркала: один Git tag
создаёт все поддерживаемые binaries, установщики и metadata.

## Канонический источник

Единственный источник релизов — `github.com/Cheviiot/Puls`. Каждый выпуск
создаётся из неизменяемого тега `vX.Y.Z`; один тег всегда соответствует одному
набору байтов.

Публичные способы установки:

1. `install.sh` для Linux и macOS;
2. `install.ps1` для Windows;
3. ручное скачивание архива из GitHub Release;
4. `go install github.com/Cheviiot/Puls/cmd/puls@latest` для пользователей Go.

Добавление внешнего package repository в текущую стратегию не входит. Оно может
стать отдельным продуктовым решением после появления реального спроса, но не
должно становиться условием установки Puls.

## Прямая установка

GitHub предоставляет постоянный URL для asset последнего выпуска:

```text
https://github.com/Cheviiot/Puls/releases/latest/download/<asset>
```

Поэтому команды установки не зависят от номера текущей версии.

Linux и macOS:

```bash
curl -fsSL https://github.com/Cheviiot/Puls/releases/latest/download/install.sh | sh
```

Windows PowerShell:

```powershell
irm https://github.com/Cheviiot/Puls/releases/latest/download/install.ps1 | iex
```

`install.sh` помещает binary в `$HOME/.local/bin`, определяет текущую оболочку и
идемпотентно настраивает `PATH` через `.bashrc`,
`.bash_profile`, `.zshrc`, `.profile` или отдельный файл Fish. Ручное
редактирование окружения и root не нужны. `install.ps1` использует
`%LOCALAPPDATA%\Programs\Puls\bin` и добавляет его в пользовательский `PATH`.
Нестандартный каталог выбирается только явным параметром пользователя.
Для автоматизации оба установщика также принимают явный override через
`PULS_INSTALL_DIR`.
Оба установщика:

- определяют `amd64` или `arm64`;
- получают только release assets из `Cheviiot/Puls`;
- выбирают ровно один архив для текущих OS и architecture из
  `RELEASE_MANIFEST.json` и проверяют ожидаемое имя пакета;
- скачивают `SHA256SUMS.txt` для того же immutable tag;
- сверяют SHA-256 пакета между manifest и `SHA256SUMS.txt`, затем проверяют
  фактически скачанные manifest и архив до распаковки;
- устанавливают binary через временный файл;
- при повторном запуске атомарно обновляют binary до `releases/latest`;
- не отправляют telemetry и не требуют GitHub token.

После загрузки скрипта удаление не делает дополнительных сетевых запросов и
затрагивает только установленный binary:

```bash
curl -fsSL https://github.com/Cheviiot/Puls/releases/latest/download/install.sh | sh -s -- --uninstall
```

```powershell
& ([scriptblock]::Create((irm https://github.com/Cheviiot/Puls/releases/latest/download/install.ps1))) -Uninstall
```

Оба установщика при удалении убирают созданную ими настройку `PATH`. При
нестандартном каталоге передайте его повторно через `--install-dir` или
`-InstallDir`. Параметры `--no-path-update` и `-NoPathUpdate` оставляют
окружение без изменений.

Для конкретной версии используются параметры `--version` и `-Version`:

```bash
sh install.sh --version 0.2.0
```

```powershell
.\install.ps1 -Version 0.2.0
```

`releases/latest` указывает на последний стабильный выпуск и не выбирает
prerelease. RC можно установить напрямую короткой командой:

```bash
curl -fsSL https://github.com/Cheviiot/Puls/releases/latest/download/install.sh | sh -s -- --version 0.2.0-rc.1
```

```powershell
& ([scriptblock]::Create((irm https://github.com/Cheviiot/Puls/releases/latest/download/install.ps1))) -Version 0.2.0-rc.1
```

Для проверки кода перед запуском установщик можно сначала скачать как обычный
файл с `releases/latest/download`, просмотреть и только затем выполнить.

## Релизный контракт

Версии и имена согласованы:

```text
Git tag:       v0.2.0
Binary:        Puls 0.2.0
Archive:       Puls_0.2.0_linux_amd64.tar.gz
Installer:     install.sh / install.ps1
```

`cmd/release` создаёт:

- Linux archives для `amd64` и `arm64`;
- Windows archives для `amd64` и `arm64`;
- macOS archives для `amd64` и `arm64`;
- `install.sh` и `install.ps1` как отдельные release assets;
- `RELEASE_MANIFEST.json` с точными архивами, OS, architecture и SHA-256;
- `SHA256SUMS.txt` для архивов, manifest и обоих установщиков.

Исходник `install.ps1` содержит только ASCII, а русский каталог сообщений
декодируется из UTF-8 во время выполнения. Asset загружается в GitHub Release с
`Content-Type: text/plain; charset=utf-8`. Это обеспечивает одинаковую работу
`irm | iex` и прямого запуска файла в Windows PowerShell 5 независимо от
системной кодовой страницы.

Каждый архив содержит binary, лицензию, changelog и полный комплект публичной
Markdown-документации. Сборка выполняется с `CGO_ENABLED=0`, `-trimpath` и
фиксированными timestamps архива.

Release workflow:

1. проверяет tag и полный test/static-analysis набор;
2. собирает артефакты;
3. проверяет `SHA256SUMS.txt`;
4. создаёт GitHub artifact attestation для всех digest;
5. загружает assets в draft release;
6. публикует release только после успешной загрузки.

Нельзя перемещать опубликованный tag, заменять assets или повторно использовать
номер версии.

### Проверка PowerShell на ALT Workstation

PowerShell не устанавливается на host. Для воспроизводимой syntax-проверки
используется отдельный Distrobox-контейнер:

```bash
distrobox create --name puls-powershell-dev \
  --image mcr.microsoft.com/powershell:7.5-ubuntu-24.04 --yes
distrobox enter puls-powershell-dev -- \
  pwsh -NoProfile -File "$PWD/scripts/install.ps1" -Help
```

Функциональный Windows integration test выполняется в CI на `windows-latest`.

## Проверка пользователем

Ручная проверка SHA-256 на Linux:

```bash
sha256sum -c SHA256SUMS.txt
```

Проверка provenance через GitHub CLI:

```bash
gh attestation verify Puls_0.2.0_linux_amd64.tar.gz --repo Cheviiot/Puls
```

SHA-256 защищает от повреждения скачанного файла. Artifact attestation
дополнительно связывает digest с release workflow репозитория Puls.

## Чек-лист выпуска

- [ ] Публичный repository имеет путь `Cheviiot/Puls`.
- [ ] Commit прошёл все обязательные проверки `main`.
- [ ] Tag имеет форму `vX.Y.Z` или `vX.Y.Z-prerelease`.
- [ ] Версия binary совпадает с tag без `v`.
- [ ] Созданы шесть базовых архивов и два установщика.
- [ ] `SHA256SUMS.txt` проверяет архивы, manifest и установщики.
- [ ] Два запуска release builder дают побайтно одинаковые файлы.
- [ ] `install.sh` проверен на Linux и macOS.
- [ ] `install.ps1` проверен на Windows `amd64` и при наличии runner — `arm64`.
- [ ] Установка не требует прав администратора.
- [ ] Удаление убирает binary, а Windows-версия — и добавленную запись `PATH`.
- [ ] `puls --version` после установки показывает версию release.
- [ ] Artifact attestation успешно проверяется для каждого asset.
- [ ] GitHub Release опубликован только после smoke tests draft-релиза.

## Границы решения

- Команды `curl | sh` и `irm | iex` не обращаются к стороннему домену: скрипты и
  binaries берутся из одного GitHub Release. Для повышенной осторожности
  пользователь может скачать и прочитать скрипт до запуска.
- Docker/OCI не используется: отдельный network namespace искажает смысл
  измерения соединения хоста.
- `linux/arm64` не объявляется Android-сборкой. Для Android потребуется
  отдельная цель и проверка среды выполнения.
- Сторонние mirrors и package repositories не считаются источником истины.

## Источники

Решение сверено 2 сентября 2026 года:

- [GitHub: ссылки на последний release и его assets](https://docs.github.com/en/repositories/releasing-projects-on-github/linking-to-releases)
- [GitHub Releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases)
- [Immutable releases](https://docs.github.com/en/repositories/releasing-projects-on-github/managing-releases-in-a-repository)
- [GitHub artifact attestations](https://docs.github.com/en/actions/concepts/security/artifact-attestations)
- [Установка Go-команд через `go install`](https://go.dev/doc/go-get-install-deprecation)
