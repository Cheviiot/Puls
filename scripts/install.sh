#!/bin/sh

set -eu

repository_url=${PULS_INSTALL_REPOSITORY_URL:-https://github.com/Cheviiot/Puls}
version=""
install_dir=${PULS_INSTALL_DIR:-}
uninstall=0

say() {
  printf '%s\n' "$*"
}

fail() {
  printf 'Ошибка: %s\n' "$*" >&2
  exit 1
}

usage() {
  cat <<'EOF'
Установка и удаление Puls через GitHub Releases

Использование:
  sh install.sh [параметры]

Параметры:
  --version <value>      установить конкретную версию, например 0.1.0
  --install-dir <path>   каталог установки · по умолчанию ~/.local/bin
  --uninstall            удалить Puls из выбранного каталога
  -h, --help             показать эту справку
EOF
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --version)
      [ "$#" -ge 2 ] || fail "параметр --version требует значение"
      version=$2
      shift 2
      ;;
    --install-dir)
      [ "$#" -ge 2 ] || fail "параметр --install-dir требует значение"
      install_dir=$2
      shift 2
      ;;
    --uninstall)
      uninstall=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail "неизвестный параметр $1"
      ;;
  esac
done

if [ -z "$install_dir" ]; then
  if [ -n "${XDG_BIN_HOME:-}" ]; then
    install_dir=$XDG_BIN_HOME
  elif [ -n "${HOME:-}" ]; then
    install_dir=$HOME/.local/bin
  else
    fail "не задан HOME; укажите каталог через --install-dir"
  fi
fi

if [ "$uninstall" -eq 1 ]; then
  target_binary=$install_dir/puls
  if [ -d "$target_binary" ]; then
    fail "$target_binary является каталогом; удаление остановлено"
  fi
  if [ -e "$target_binary" ] || [ -L "$target_binary" ]; then
    rm -f -- "$target_binary" || fail "не удалось удалить $target_binary"
    say "Puls удалён: $target_binary"
  else
    say "Puls уже удалён: $target_binary"
  fi
  exit 0
fi

command -v curl >/dev/null 2>&1 || fail "не найден curl"
command -v tar >/dev/null 2>&1 || fail "не найден tar"
command -v mktemp >/dev/null 2>&1 || fail "не найден mktemp"

case "$repository_url" in
  https://github.com/*)
    secure_download=1
    ;;
  http://127.0.0.1:*|http://localhost:*)
    # Локальный HTTP разрешён только для integration tests.
    secure_download=0
    ;;
  *)
    fail "источник установки должен использовать github.com по HTTPS"
    ;;
esac
repository_url=${repository_url%/}

download() {
  source_url=$1
  destination=$2
  if [ "$secure_download" -eq 1 ]; then
    curl --proto '=https' --tlsv1.2 --fail --silent --show-error --location \
      --output "$destination" "$source_url"
  else
    curl --fail --silent --show-error --location \
      --output "$destination" "$source_url"
  fi
}

latest_url() {
  if [ "$secure_download" -eq 1 ]; then
    curl --proto '=https' --tlsv1.2 --fail --silent --show-error --location \
      --output /dev/null --write-out '%{url_effective}' \
      "$repository_url/releases/latest"
  else
    curl --fail --silent --show-error --location --output /dev/null \
      --write-out '%{url_effective}' "$repository_url/releases/latest"
  fi
}

case $(uname -s) in
  Linux) target_os=linux ;;
  Darwin) target_os=darwin ;;
  *) fail "поддерживаются только Linux и macOS" ;;
esac

case $(uname -m) in
  x86_64|amd64) target_arch=amd64 ;;
  arm64|aarch64) target_arch=arm64 ;;
  *) fail "неподдерживаемая архитектура $(uname -m)" ;;
esac

if [ -z "$version" ]; then
  resolved_url=$(latest_url) || fail "не удалось определить последний релиз"
  tag=${resolved_url%/}
  tag=${tag##*/}
  version=${tag#v}
else
  version=${version#v}
fi

case "$version" in
  ""|.|..|*[!0-9A-Za-z._-]*) fail "некорректная версия $version" ;;
esac

asset="Puls_${version}_${target_os}_${target_arch}.tar.gz"
release_url="$repository_url/releases/download/v${version}"
temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/puls-install.XXXXXXXX") || \
  fail "не удалось создать временный каталог"
staged_binary=""
cleanup() {
  rm -rf -- "$temporary_dir"
  if [ -n "$staged_binary" ]; then
    rm -f -- "$staged_binary"
  fi
}
trap cleanup EXIT
trap 'exit 130' HUP INT TERM

archive_path=$temporary_dir/$asset
checksums_path=$temporary_dir/SHA256SUMS.txt
say "Загрузка Puls $version для $target_os/$target_arch…"
download "$release_url/$asset" "$archive_path" || fail "не удалось скачать $asset"
download "$release_url/SHA256SUMS.txt" "$checksums_path" || \
  fail "не удалось скачать SHA256SUMS.txt"

checksum_count=$(awk -v name="$asset" '$2 == name { count++ } END { print count + 0 }' "$checksums_path")
[ "$checksum_count" -eq 1 ] || fail "в SHA256SUMS.txt нет единственной записи для $asset"
expected_checksum=$(awk -v name="$asset" '$2 == name { print $1 }' "$checksums_path")
case "$expected_checksum" in
  *[!0-9A-Fa-f]*) fail "в SHA256SUMS.txt указан некорректный SHA-256" ;;
esac
[ "${#expected_checksum}" -eq 64 ] || fail "в SHA256SUMS.txt указан некорректный SHA-256"

if command -v sha256sum >/dev/null 2>&1; then
  actual_checksum=$(sha256sum "$archive_path" | awk '{ print $1 }')
elif command -v shasum >/dev/null 2>&1; then
  actual_checksum=$(shasum -a 256 "$archive_path" | awk '{ print $1 }')
else
  fail "не найден sha256sum или shasum"
fi
expected_checksum=$(printf '%s' "$expected_checksum" | tr '[:upper:]' '[:lower:]')
actual_checksum=$(printf '%s' "$actual_checksum" | tr '[:upper:]' '[:lower:]')
[ "$actual_checksum" = "$expected_checksum" ] || fail "контрольная сумма архива не совпала"

extract_dir=$temporary_dir/extracted
mkdir -p "$extract_dir"
tar -xzf "$archive_path" -C "$extract_dir"
binary_path="$extract_dir/Puls_${version}_${target_os}_${target_arch}/puls"
[ -f "$binary_path" ] || fail "в архиве не найден puls"

mkdir -p "$install_dir"
staged_binary=$(mktemp "$install_dir/.puls.XXXXXXXX") || fail "каталог установки недоступен: $install_dir"
cp "$binary_path" "$staged_binary"
chmod 0755 "$staged_binary"
mv -f "$staged_binary" "$install_dir/puls"
staged_binary=""

say "Puls $version установлен: $install_dir/puls"
case ":${PATH:-}:" in
  *:"$install_dir":*) ;;
  *)
    say "Добавьте $install_dir в PATH, затем откройте новый терминал."
    ;;
esac
