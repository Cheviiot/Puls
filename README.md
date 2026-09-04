<div align="center">

# Puls

**Проверка скорости и качества интернета из терминала**

Яндекс.Интернетометр · speedtest.ru · Linux · macOS · Windows

[![CI](https://github.com/Cheviiot/Puls/actions/workflows/ci.yml/badge.svg)](https://github.com/Cheviiot/Puls/actions/workflows/ci.yml)
[![Go](https://img.shields.io/badge/Go-1.26.7%2B-00ADD8?logo=go&logoColor=white)](https://go.dev/)
[![MIT](https://img.shields.io/badge/license-MIT-2ea44f.svg)](LICENSE)

</div>

Puls измеряет задержку, скачивание и отдачу через публичные протоколы
официальных веб-клиентов.

## Установка

Linux и macOS:

```sh
curl -fsSL https://github.com/Cheviiot/Puls/releases/latest/download/install.sh | sh
```

Windows PowerShell:

```powershell
irm https://github.com/Cheviiot/Puls/releases/latest/download/install.ps1 | iex
```

Установщик проверяет SHA-256 и настраивает пользовательский `PATH`.

> **Обновление:** повторно выполните команду установки для своей системы.

Удаление:

```sh
curl -fsSL https://github.com/Cheviiot/Puls/releases/latest/download/install.sh | sh -s -- --uninstall
```

```powershell
& ([scriptblock]::Create((irm https://github.com/Cheviiot/Puls/releases/latest/download/install.ps1))) -Uninstall
```

## Использование

```sh
puls                         # выбрать сервис в терминале
puls yandex                  # полное измерение через Яндекс
puls speedtest --only ping   # только задержка
puls all --profile quick     # оба сервиса последовательно
puls ip                      # внешний IP и интернет-провайдер
puls all --json              # машиночитаемый результат
```

| Параметр | Значение |
| --- | --- |
| `--profile` | `quick`, `balanced` или `accurate` |
| `--duration` | от 3 до 60 секунд |
| `--connections` | `0` — автоматически, вручную — от 1 до 16 |
| `--only` | `all`, `ping`, `download` или `upload` |
| `--server` | сервер измерения для `speedtest` |
| `--show-ip` | добавить данные подключения |
| `--json` | вывести JSON |
| `--verbose` | показать диагностику в `stderr` |
| `--no-color` | отключить цвет |

Полная справка: `puls help`.

## Сервисы

| Сервис | Ping | Download | Upload | IP | Интернет-провайдер |
| --- | :---: | :---: | :---: | :---: | :---: |
| `yandex` | ✓ | ✓ | ✓ | ✓ | — |
| `speedtest` | ✓ | ✓ | ✓ | ✓ | ✓ |

`puls ip` сначала обращается к speedtest.ru, затем к Яндексу. Ошибка одного
сервиса не останавливает `puls all`.

## Важно

- Puls не отправляет телеметрию и не записывает IP, browser key или JWT на диск.
- Одна фаза расходует примерно `скорость × длительность ÷ 8` данных.
- Проект независимый и не связан с Яндексом или speedtest.ru.

Методики основаны на [Яндекс.Интернетометре](https://yandex.ru/support2/internet/ru/measure)
и [speedtest.ru](https://speedtest.ru/manual). Инженерные подробности:
[архитектура](docs/architecture.md), [сборка и выпуск](docs/distribution.md),
[инструкции для AI-разработчиков](AGENTS.md).

## Разработка

```sh
go test ./...
go test -race ./...
go vet ./...
```

Участие в проекте: [CONTRIBUTING.md](.github/CONTRIBUTING.md).

## Лицензия

[MIT](LICENSE)
