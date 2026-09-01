#!/bin/sh

set -eu

repository_url=${PULS_INSTALL_REPOSITORY_URL:-https://github.com/Cheviiot/Puls}
version=""
install_dir=${PULS_INSTALL_DIR:-}
uninstall=0
update_path=1
temporary_dir=""
staged_binary=""
profile_temp=""
profile_file=""
profile_line=""
path_cleanup_changed=0
profile_line_removed=0

say() {
  printf '%s\n' "$*"
}

fail() {
  printf 'Ошибка: %s\n' "$*" >&2
  exit 1
}

cleanup() {
  cleanup_status=$?
  trap - EXIT
  if [ -n "$temporary_dir" ]; then
    rm -rf -- "$temporary_dir" || :
  fi
  if [ -n "$staged_binary" ]; then
    rm -f -- "$staged_binary" || :
  fi
  if [ -n "$profile_temp" ]; then
    rm -f -- "$profile_temp" || :
  fi
  exit "$cleanup_status"
}

path_contains() {
  case ":${PATH:-}:" in
    *:"$1":*) return 0 ;;
    *) return 1 ;;
  esac
}

validate_path_entry() {
  cleaned_path=$(printf '%s' "$install_dir" | tr -d '\n\r')
  [ "$cleaned_path" = "$install_dir" ] || \
    fail "каталог установки не должен содержать перевод строки"
  case "$install_dir" in
    *:*) fail "каталог установки для PATH не должен содержать двоеточие" ;;
    *"'"*) fail "каталог установки для PATH не должен содержать одинарную кавычку" ;;
  esac
}

prepare_path_update() {
  [ "$update_path" -eq 1 ] || return 0
  path_contains "$install_dir" && return 0
  [ -n "${HOME:-}" ] || \
    fail "не задан HOME; используйте --no-path-update или настройте HOME"
  validate_path_entry

  configured_shell=${SHELL:-}
  shell_name=${configured_shell##*/}
  case "$shell_name" in
    fish)
      profile_file=${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/puls.fish
      profile_line="fish_add_path -- '$install_dir' # Puls installer"
      ;;
    zsh)
      profile_file=$HOME/.zshrc
      profile_line="export PATH='$install_dir':\"\$PATH\" # Puls installer"
      ;;
    bash)
      if [ "$(uname -s)" = Darwin ]; then
        profile_file=$HOME/.bash_profile
      else
        profile_file=$HOME/.bashrc
      fi
      profile_line="export PATH='$install_dir':\"\$PATH\" # Puls installer"
      ;;
    *)
      profile_file=$HOME/.profile
      profile_line="export PATH='$install_dir':\"\$PATH\" # Puls installer"
      ;;
  esac
}

add_path_configuration() {
  [ "$update_path" -eq 1 ] || return 0
  PATH=$install_dir:${PATH:-}
  export PATH
  if [ -z "$profile_file" ]; then
    say "Puls готов к работе: puls --help"
    return 0
  fi

  profile_parent=${profile_file%/*}
  mkdir -p "$profile_parent"
  [ ! -d "$profile_file" ] || fail "$profile_file является каталогом"
  : >> "$profile_file"
  profile_line_found=0
  while IFS= read -r existing_line || [ -n "$existing_line" ]; do
    if [ "$existing_line" = "$profile_line" ]; then
      profile_line_found=1
      break
    fi
  done < "$profile_file"
  if [ "$profile_line_found" -eq 0 ]; then
    if [ -s "$profile_file" ] && \
      [ "$(tail -c 1 "$profile_file" | wc -l | tr -d '[:space:]')" -eq 0 ]; then
      printf '\n%s\n' "$profile_line" >> "$profile_file" || \
        fail "не удалось обновить $profile_file"
    else
      printf '%s\n' "$profile_line" >> "$profile_file" || \
        fail "не удалось обновить $profile_file"
    fi
  fi
  say "PATH настроен в $profile_file. Откройте новый терминал и запустите puls --help."
}

remove_profile_line() {
  remove_file=$1
  remove_line=$2
  profile_line_removed=0
  [ -f "$remove_file" ] || return 0

  profile_temp=$(mktemp "${remove_file}.puls.XXXXXXXX") || \
    fail "не удалось подготовить обновление $remove_file"
  cp -p "$remove_file" "$profile_temp" || fail "не удалось прочитать $remove_file"
  : > "$profile_temp"
  removed_line=0
  while IFS= read -r existing_line || [ -n "$existing_line" ]; do
    if [ "$existing_line" = "$remove_line" ]; then
      removed_line=1
    else
      printf '%s\n' "$existing_line" >> "$profile_temp" || \
        fail "не удалось обновить $remove_file"
    fi
  done < "$remove_file"

  if [ "$removed_line" -eq 1 ]; then
    if [ -L "$remove_file" ]; then
      cat "$profile_temp" > "$remove_file" || fail "не удалось обновить $remove_file"
      rm -f -- "$profile_temp"
    else
      mv -f "$profile_temp" "$remove_file" || fail "не удалось обновить $remove_file"
    fi
    path_cleanup_changed=1
    profile_line_removed=1
  else
    rm -f -- "$profile_temp"
  fi
  profile_temp=""
}

remove_path_configuration() {
  [ "$update_path" -eq 1 ] || return 0
  [ -n "${HOME:-}" ] || return 0
  validate_path_entry
  posix_path_line="export PATH='$install_dir':\"\$PATH\" # Puls installer"
  fish_path_line="fish_add_path -- '$install_dir' # Puls installer"
  remove_profile_line "$HOME/.bashrc" "$posix_path_line"
  remove_profile_line "$HOME/.bash_profile" "$posix_path_line"
  remove_profile_line "$HOME/.zshrc" "$posix_path_line"
  remove_profile_line "$HOME/.profile" "$posix_path_line"
  fish_profile=${XDG_CONFIG_HOME:-$HOME/.config}/fish/conf.d/puls.fish
  remove_profile_line "$fish_profile" "$fish_path_line"
  if [ "$profile_line_removed" -eq 1 ] && [ ! -s "$fish_profile" ]; then
    rm -f -- "$fish_profile" || fail "не удалось удалить $fish_profile"
  fi
  if [ "$path_cleanup_changed" -eq 1 ]; then
    say "Настройка PATH удалена."
  fi
}

usage() {
  cat <<'EOF'
Установка и удаление Puls через GitHub Releases

Использование:
  sh install.sh [параметры]

Параметры:
  --version <value>      установить конкретную версию, например 0.1.0
  --install-dir <path>   каталог установки · по умолчанию ~/.local/bin
  --no-path-update       не изменять конфигурацию командной оболочки
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
    --no-path-update)
      update_path=0
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
    for candidate_dir in "$HOME/.local/bin" "$HOME/bin"; do
      if path_contains "$candidate_dir"; then
        install_dir=$candidate_dir
        break
      fi
    done
  else
    fail "не задан HOME; укажите каталог через --install-dir"
  fi
fi

case "$install_dir" in
  /*) ;;
  *) install_dir=$(pwd -P)/$install_dir ;;
esac
if [ "$install_dir" != / ]; then
  install_dir=${install_dir%/}
fi

trap cleanup EXIT
trap 'exit 130' HUP INT TERM

if [ "$uninstall" -eq 1 ]; then
  if [ "$update_path" -eq 1 ]; then
    command -v mktemp >/dev/null 2>&1 || fail "не найден mktemp"
  fi
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
  if [ "$update_path" -eq 1 ]; then
    remove_path_configuration
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

prepare_path_update

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

archive_path=$temporary_dir/$asset
checksums_path=$temporary_dir/SHA256SUMS.txt
say "Загрузка Puls ${version} для ${target_os}/${target_arch}..."
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
target_binary=$install_dir/puls
[ ! -d "$target_binary" ] || fail "$target_binary является каталогом"
install_action=установлен
if [ -e "$target_binary" ] || [ -L "$target_binary" ]; then
  install_action=обновлён
fi
staged_binary=$(mktemp "$install_dir/.puls.XXXXXXXX") || fail "каталог установки недоступен: $install_dir"
cp "$binary_path" "$staged_binary"
chmod 0755 "$staged_binary"
mv -f "$staged_binary" "$target_binary"
staged_binary=""

say "Puls $version $install_action: $target_binary"
add_path_configuration
