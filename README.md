<div align="center">

# Puls

**Проверка скорости интернета в приложении и терминале**

[![CI](https://github.com/Cheviiot/Puls/actions/workflows/ci.yml/badge.svg)](https://github.com/Cheviiot/Puls/actions/workflows/ci.yml)
[![MIT](https://img.shields.io/badge/license-MIT-2ea44f.svg)](LICENSE)

</div>

Puls измеряет задержку, загрузку и отдачу через Яндекс.Интернетометр и
speedtest.ru. Доступны единый GUI для компьютера и Android, а также CLI.

## Установка

Linux и macOS:

```sh
curl -fsSL https://github.com/Cheviiot/Puls/releases/latest/download/install.sh | sh
```

Windows PowerShell:

```powershell
irm https://github.com/Cheviiot/Puls/releases/latest/download/install.ps1 | iex
```

Установщик добавляет `puls` в `PATH` и создаёт ярлык приложения. Android APK
доступен в [последнем релизе](https://github.com/Cheviiot/Puls/releases/latest).

> Для обновления повторно выполните ту же команду установки.

## Запуск

```sh
puls gui                     # графический интерфейс
puls yandex                  # измерение через Яндекс
puls speedtest --only ping   # только задержка
puls all --profile quick     # оба сервиса последовательно
puls ip                      # внешний IP и интернет-провайдер
puls help                    # параметры CLI
```

Удаление:

```sh
curl -fsSL https://github.com/Cheviiot/Puls/releases/latest/download/install.sh | sh -s -- --uninstall
```

```powershell
& ([scriptblock]::Create((irm https://github.com/Cheviiot/Puls/releases/latest/download/install.ps1))) -Uninstall
```

Puls не отправляет телеметрию и не сохраняет IP, результаты измерений, JWT или
browser keys. Проект независимый и распространяется по лицензии [MIT](LICENSE).

Разработка: [CONTRIBUTING](.github/CONTRIBUTING.md) ·
[архитектура](docs/architecture.md) · [выпуск](docs/distribution.md)
