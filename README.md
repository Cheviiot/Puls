<div align="center">

# Puls

**Честный замер скорости интернета из терминала**

Яндекс.Интернетометр и speedtest.ru · Linux, macOS и Windows

[![CI](https://github.com/Cheviiot/Puls/actions/workflows/ci.yml/badge.svg)](https://github.com/Cheviiot/Puls/actions/workflows/ci.yml)
[![Go](https://img.shields.io/badge/Go-1.26.7%2B-00ADD8?logo=go&logoColor=white)](https://go.dev/)
[![MIT](https://img.shields.io/badge/license-MIT-2ea44f.svg)](LICENSE)

</div>

Puls измеряет задержку, скачивание и отдачу по методикам официальных
веб-клиентов. Сетевой сбой показывается как ошибка, а не как успешные
`0 Мбит/с`.

## Установка

Linux и macOS:

```sh
curl -fsSL https://github.com/Cheviiot/Puls/releases/latest/download/install.sh|sh
```

Windows PowerShell:

```powershell
irm https://github.com/Cheviiot/Puls/releases/latest/download/install.ps1|iex
```

Скрипты скачиваются только из GitHub Releases, проверяют SHA-256, сами
настраивают `PATH` и не требуют прав администратора. В уже открытом Unix-окне
после первой установки достаточно открыть новую вкладку терминала. Готовый
архив можно взять вручную на странице [Releases](https://github.com/Cheviiot/Puls/releases).

### Обновление

Повторите команду установки. Puls скачает `releases/latest`, проверит архив и
атомарно заменит предыдущую версию, не дублируя настройку `PATH`.

```sh
curl -fsSL https://github.com/Cheviiot/Puls/releases/latest/download/install.sh|sh
```

```powershell
irm https://github.com/Cheviiot/Puls/releases/latest/download/install.ps1|iex
```

### Удаление

```sh
curl -fsSL https://github.com/Cheviiot/Puls/releases/latest/download/install.sh|sh -s -- --uninstall
```

```powershell
& ([scriptblock]::Create((irm https://github.com/Cheviiot/Puls/releases/latest/download/install.ps1))) -Uninstall
```

## Использование

```text
puls [source] [options]
```

```sh
puls
puls yandex --profile quick
puls speedtest --only ping
puls all --json
```

Источники: `yandex`, `speedtest` и `all`. По умолчанию Puls последовательно
проверяет оба источника в профиле `balanced`.

| Option | Значение |
| --- | --- |
| `--profile quick\|balanced\|accurate` | 5, 10 или 15 секунд на каждую фазу |
| `--duration 3..60` | Явная длительность фазы |
| `--connections 0..16` | `0` — нативный автоматический режим |
| `--only all\|ping\|download\|upload` | Какие измерения выполнить |
| `--server <host>` | Сервер только для `speedtest` |
| `--json` | Машинный результат без оформления |
| `--verbose` | Выбор endpoint и fallback в `stderr` |
| `--no-color` | Вывод без цвета |
| `--version` | Версия Puls |
| `-h`, `--help` | Краткая справка |

Пример результата:

```text
ЯНДЕКС
────────────────────────────────────────────
Сервер      ext-cloudcdn-ruvld01rtk-01.cdn.yandex.net
Задержка    19.0 мс  ·  джиттер 1.4 мс
Скачивание  89.68 Мбит/с
Отдача      93.29 Мбит/с
```

## Источники

| Source | Сервер | Ping | Download | Upload |
| --- | --- | :---: | :---: | :---: |
| `yandex` | Ближайшие CDN из `get-probes` | ✓ | ✓ | ✓ |
| `speedtest` | Минимальная медианная задержка | ✓ | ✓ | ✓ |
| `all` | Оба источника последовательно | ✓ | ✓ | ✓ |

Puls проверяет status, schema, WebSocket frames и точный размер payload.
Учитываются только байты, подтверждённые протоколом или успешным HTTP-ответом.
Warm-up не входит в результат, worker может переподключиться один раз, а сбой
одного источника не останавливает `all`.

## JSON и коды завершения

`--json` пишет результат в `stdout`; подробности `--verbose` остаются в
`stderr`. Пропущенные измерения представлены как `null`.

```json
{
  "provider": "yandex",
  "server": "ext-cloudcdn-ruvld01rtk-01.cdn.yandex.net",
  "status": "ok",
  "ping_ms": 19.0,
  "jitter_ms": 1.4,
  "download_mbps": 89.68,
  "upload_mbps": 93.29
}
```

| Code | Значение |
| ---: | --- |
| `0` | Успех |
| `1` | Ошибка источника, partial result или ошибка JSON |
| `2` | Некорректные аргументы |
| `130` | Остановка через Ctrl+C |

## Трафик и приватность

Одна фаза передаёт примерно `скорость × длительность ÷ 8`. При 100 Мбит/с
профиль `balanced` использует около 125 МБ на скачивание и до 125 МБ на отдачу,
не считая небольшого warm-up.

Puls не отправляет телеметрию и результаты замеров, не хранит JWT или IP-ответы
и не обходит CAPTCHA, anti-bot и ограничения доступа. Проект независимый и не
связан с Яндексом или speedtest.ru.

## Разработка

```sh
go test ./...
go test -race ./...
go vet ./...
```

- [Архитектура](docs/architecture.md)
- [Сборка и выпуск](docs/distribution.md)
- [Правила участия](.github/CONTRIBUTING.md)
- [Безопасность](.github/SECURITY.md)
- [Инструкции для AI-разработчиков](AGENTS.md)
- [История изменений](CHANGELOG.md)

## Лицензия

[MIT](LICENSE)
