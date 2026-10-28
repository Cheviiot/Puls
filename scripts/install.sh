#!/bin/sh

set -eu

repository_url=${PULS_INSTALL_REPOSITORY_URL:-https://github.com/Cheviiot/Puls}
version=""
install_dir=${PULS_INSTALL_DIR:-}
uninstall=0
update_path=1
install_shortcut=1
temporary_dir=""
staged_binary=""
profile_temp=""
shortcut_temp=""
profile_file=""
profile_line=""
path_cleanup_changed=0
profile_line_removed=0
runtime_stage=""
runtime_backup_version=""
runtime_backup_current=""
runtime_backup_launcher=""
runtime_pointer_temp=""
runtime_launcher_temp=""
runtime_committed=0
runtime_new_version=""
runtime_version_installed=0
runtime_version_backup_ready=0
runtime_pointer_changed=0
runtime_pointer_backup_ready=0
runtime_launcher_changed=0
runtime_launcher_backup_ready=0

say() {
  printf '%s\n' "$*"
}

fail() {
  printf 'Ошибка: %s\n' "$*" >&2
  exit 1
}

runtime_owned() {
  owned_dir=$1
  owned_version=$2
  [ ! -L "$owned_dir" ] && [ -d "$owned_dir" ] && \
    [ ! -L "$owned_dir/.puls-runtime-owner" ] && \
    [ -f "$owned_dir/.puls-runtime-owner" ] && \
    [ "$(cat "$owned_dir/.puls-runtime-owner" 2>/dev/null)" = "puls-installer-v1:$owned_version" ]
}

ensure_real_directory() {
  ensure_dir=$1
  if [ -L "$ensure_dir" ] || { [ -e "$ensure_dir" ] && [ ! -d "$ensure_dir" ]; }; then
    fail "$ensure_dir уже существует и не является безопасным каталогом"
  fi
  mkdir -p "$ensure_dir"
}

validate_archive_members() {
  validate_archive=$1
  validate_root=$2
  tar -tzf "$validate_archive" | awk -v root="$validate_root" '
    BEGIN { prefix = root "/" }
    {
      name = $0
      if (name == "" || name ~ /[\\\\\r\n]/ || name ~ /^\// || name ~ /^[A-Za-z]:/ || name ~ /(^|\/)\.\.(\/|$)/ || name ~ /(^|\/)\.\//) exit 1
      if (name != root && index(name, prefix) != 1) exit 1
    }
  ' || fail "архив содержит небезопасные пути"
}

validate_archive_links() {
  links_archive=$1
  links_root=$2
  tar -tvzf "$links_archive" | awk -v root="$links_root" '
    $1 ~ /^h/ { exit 1 }
    $1 ~ /^l/ {
      arrow = index($0, " -> ")
      if (arrow == 0) exit 1
      metadata = substr($0, 1, arrow - 1)
      target = substr($0, arrow + 4)
      needle = root "/"
      start = index(metadata, needle)
      if (start == 0 || target == "" || target ~ /^\// || target ~ /^[A-Za-z]:/ || target ~ /[\\\\]/) exit 1
      relative = substr(metadata, start + length(needle))
      base = relative
      if (sub(/\/[^\/]*$/, "", base) == 0) base = ""
      depth = 0
      count = split(base, parts, "/")
      for (i = 1; i <= count; i++) if (parts[i] != "" && parts[i] != ".") stack[++depth] = parts[i]
      count = split(target, parts, "/")
      for (i = 1; i <= count; i++) {
        if (parts[i] == "" || parts[i] == ".") continue
        if (parts[i] == "..") { if (depth == 0) exit 1; depth-- }
        else stack[++depth] = parts[i]
      }
    }
  ' || fail "архив содержит небезопасные ссылки"
}

runtime_pointer_target() {
  [ -L "$1" ] || return 1
  pointer_value=$(readlink "$1") || return 1
  case "$pointer_value" in
    versions/*) pointer_version=${pointer_value#versions/} ;;
    *) return 1 ;;
  esac
  case "$pointer_version" in
    ""|.|..|*/*|*[!0-9A-Za-z._-]*) return 1 ;;
  esac
  runtime_owned "$runtime_versions/$pointer_version" "$pointer_version"
}

replace_symlink_atomically() {
  replacement=$1
  destination=$2
  if [ "$target_os" = darwin ]; then
    mv -fh "$replacement" "$destination"
  else
    mv -Tf "$replacement" "$destination"
  fi
}

install_runtime_tree() {
  runtime_source=$1
  runtime_executable=$2
  runtime_root=$install_dir/.puls-runtime
  runtime_versions=$runtime_root/versions
  runtime_current=$runtime_root/current
  runtime_version_dir=$runtime_versions/$version
  runtime_stage=$runtime_versions/.stage.$$
  runtime_new_version=$runtime_version_dir
  runtime_backup_version=$runtime_versions/.backup.$$.version
  runtime_backup_current=$runtime_root/.backup.$$.current
  runtime_backup_launcher=$install_dir/.puls-backup.$$.launcher

  ensure_real_directory "$install_dir"
  ensure_real_directory "$runtime_root"
  ensure_real_directory "$runtime_versions"
  if [ -e "$runtime_stage" ] || [ -L "$runtime_stage" ]; then
    fail "временный runtime-каталог уже существует"
  fi
  cp -R "$runtime_source" "$runtime_stage" || fail "не удалось подготовить runtime"
  printf 'puls-installer-v1:%s\n' "$version" > "$runtime_stage/.puls-runtime-owner" || fail "не удалось пометить runtime"
  if [ ! -f "$runtime_stage/$runtime_executable" ] || [ -L "$runtime_stage/$runtime_executable" ]; then
    fail "в runtime отсутствует исполняемый файл"
  fi
  chmod 0755 "$runtime_stage/$runtime_executable"
  "$runtime_stage/$runtime_executable" --version >/dev/null 2>&1 || \
    fail "runtime не прошёл проверку --version"

  if [ -e "$runtime_current" ] || [ -L "$runtime_current" ]; then
    runtime_pointer_target "$runtime_current" || fail "указатель runtime существует, но не принадлежит установщику Puls"
    runtime_pointer_backup_ready=1
    ln -s "$(readlink "$runtime_current")" "$runtime_backup_current" || fail "не удалось сохранить прежний runtime"
  fi

  if [ -e "$runtime_version_dir" ] || [ -L "$runtime_version_dir" ]; then
    runtime_owned "$runtime_version_dir" "$version" || fail "версия runtime уже существует и не принадлежит установщику Puls"
    runtime_version_backup_ready=1
    mv "$runtime_version_dir" "$runtime_backup_version" || fail "не удалось сохранить прежнюю версию runtime"
  fi
  runtime_version_installed=1
  mv "$runtime_stage" "$runtime_version_dir" || fail "не удалось установить runtime"
  runtime_stage=""

  runtime_pointer_temp=$runtime_root/.current.$$.tmp
  ln -s "versions/$version" "$runtime_pointer_temp" || fail "не удалось подготовить указатель runtime"
  runtime_pointer_changed=1
  replace_symlink_atomically "$runtime_pointer_temp" "$runtime_current" || fail "не удалось переключить runtime"
  runtime_pointer_temp=""

  target_binary=$install_dir/puls
  if [ -e "$target_binary" ] || [ -L "$target_binary" ]; then
    if [ -L "$target_binary" ]; then
      case "$(readlink "$target_binary")" in
        .puls-runtime/current/*) ;;
        *) fail "$target_binary уже существует и не принадлежит установщику Puls" ;;
      esac
      runtime_launcher_backup_ready=1
      ln -s "$(readlink "$target_binary")" "$runtime_backup_launcher" || fail "не удалось сохранить прежнюю команду puls"
    elif [ -f "$target_binary" ]; then
      runtime_launcher_backup_ready=1
      mv "$target_binary" "$runtime_backup_launcher" || fail "не удалось сохранить прежнюю команду puls"
    else
      fail "$target_binary не является обычным файлом"
    fi
  fi
  runtime_launcher_temp=$install_dir/.puls.$$.tmp
  case "$target_os:$runtime_executable" in
    darwin:puls.app/Contents/MacOS/puls) launcher_target=.puls-runtime/current/puls.app/Contents/MacOS/puls ;;
    *) launcher_target=.puls-runtime/current/bin/puls ;;
  esac
  ln -s "$launcher_target" "$runtime_launcher_temp" || fail "не удалось подготовить команду puls"
  runtime_launcher_changed=1
  replace_symlink_atomically "$runtime_launcher_temp" "$target_binary" || fail "не удалось установить команду puls"
  runtime_launcher_temp=""
  runtime_committed=1
  rm -f -- "$runtime_backup_current" "$runtime_backup_launcher" 2>/dev/null || :
  if [ -n "$runtime_backup_version" ] && [ -e "$runtime_backup_version" ]; then
    rm -rf -- "$runtime_backup_version" || :
  fi
  for old_runtime in "$runtime_versions"/*; do
    [ -e "$old_runtime" ] || continue
    old_version=${old_runtime##*/}
    [ "$old_version" = "$version" ] && continue
    if runtime_owned "$old_runtime" "$old_version"; then
      rm -rf -- "$old_runtime" || :
    fi
  done
  runtime_backup_current=""
  runtime_backup_launcher=""
  runtime_backup_version=""
  runtime_new_version=""
  runtime_version_installed=0
  runtime_version_backup_ready=0
  runtime_pointer_changed=0
  runtime_pointer_backup_ready=0
  runtime_launcher_changed=0
  runtime_launcher_backup_ready=0
}

cleanup() {
  cleanup_status=$?
  trap - EXIT
  if [ "$runtime_committed" -ne 1 ]; then
    if [ "$runtime_launcher_changed" -eq 1 ]; then
      rm -f -- "$install_dir/puls" || :
    fi
    if [ "$runtime_launcher_backup_ready" -eq 1 ] && { [ -e "$runtime_backup_launcher" ] || [ -L "$runtime_backup_launcher" ]; }; then
      if [ -L "$runtime_backup_launcher" ]; then
        replace_symlink_atomically "$runtime_backup_launcher" "$install_dir/puls" || :
      else
        mv "$runtime_backup_launcher" "$install_dir/puls" || :
      fi
    fi
    if [ "$runtime_pointer_changed" -eq 1 ]; then
      rm -f -- "$install_dir/.puls-runtime/current" || :
    fi
    if [ "$runtime_version_installed" -eq 1 ] && [ -n "$runtime_new_version" ] && runtime_owned "$runtime_new_version" "$version"; then
      rm -rf -- "$runtime_new_version" || :
    fi
    if [ "$runtime_version_backup_ready" -eq 1 ] && [ -e "$runtime_backup_version" ]; then
      rm -rf -- "${runtime_versions:?}/$version" || :
      mv "$runtime_backup_version" "$runtime_versions/$version" || :
    fi
    if [ "$runtime_pointer_backup_ready" -eq 1 ] && { [ -e "$runtime_backup_current" ] || [ -L "$runtime_backup_current" ]; }; then
      replace_symlink_atomically "$runtime_backup_current" "$install_dir/.puls-runtime/current" || :
    fi
  fi
  if [ -n "$temporary_dir" ]; then
    rm -rf -- "$temporary_dir" || :
  fi
  if [ -n "$staged_binary" ]; then
    rm -f -- "$staged_binary" || :
  fi
  if [ -n "$profile_temp" ]; then
    rm -f -- "$profile_temp" || :
  fi
  if [ -n "$shortcut_temp" ]; then
    rm -f -- "$shortcut_temp" || :
  fi
  if [ -n "$runtime_stage" ] && [ -e "$runtime_stage" ]; then rm -rf -- "$runtime_stage" || :; fi
  if [ -n "$runtime_pointer_temp" ]; then rm -f -- "$runtime_pointer_temp" || :; fi
  if [ -n "$runtime_launcher_temp" ]; then rm -f -- "$runtime_launcher_temp" || :; fi
  if [ -n "${runtime_backup_current:-}" ] && [ -e "$runtime_backup_current" ] && [ "$runtime_committed" -eq 1 ]; then rm -f -- "$runtime_backup_current" || :; fi
  if [ -n "${runtime_backup_launcher:-}" ] && [ -e "$runtime_backup_launcher" ] && [ "$runtime_committed" -eq 1 ]; then rm -f -- "$runtime_backup_launcher" || :; fi
  if [ -n "${runtime_backup_version:-}" ] && [ -e "$runtime_backup_version" ] && [ "$runtime_committed" -eq 1 ]; then rm -rf -- "$runtime_backup_version" || :; fi
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

remove_gui_shortcut() {
  [ "$install_shortcut" -eq 1 ] || return 0
  [ -n "${HOME:-}" ] || return 0
  case "$target_os" in
    linux)
      data_home=${XDG_DATA_HOME:-$HOME/.local/share}
      desktop_file=$data_home/applications/io.github.cheviiot.puls.desktop
      if [ -f "$desktop_file" ] && grep -Fqx 'X-Puls-Managed=true' "$desktop_file"; then
        rm -f -- "$desktop_file" || fail "не удалось удалить $desktop_file"
        rm -f -- "$data_home/icons/hicolor/512x512/apps/io.github.cheviiot.puls.png" || :
        say "Ярлык Puls удалён."
      fi
      ;;
    darwin)
      app_bundle=$HOME/Applications/Puls.app
      if macos_shortcut_is_managed; then
        rm -rf -- "$app_bundle" || fail "не удалось удалить $app_bundle"
        say "Приложение Puls удалено из ~/Applications."
      fi
      ;;
  esac
}

remove_managed_runtime() {
  runtime_root=$install_dir/.puls-runtime
  runtime_versions=$runtime_root/versions
  runtime_current=$runtime_root/current
  if [ -L "$runtime_current" ]; then
    if runtime_pointer_target "$runtime_current"; then
      rm -f -- "$runtime_current" || fail "не удалось удалить указатель runtime"
    fi
  elif [ -e "$runtime_current" ]; then
    fail "$runtime_current не принадлежит установщику Puls"
  fi
  if [ -d "$runtime_versions" ] && [ ! -L "$runtime_versions" ]; then
    for runtime_version_dir in "$runtime_versions"/*; do
      [ -e "$runtime_version_dir" ] || continue
      runtime_basename=${runtime_version_dir##*/}
      if runtime_owned "$runtime_version_dir" "$runtime_basename"; then
        rm -rf -- "$runtime_version_dir" || fail "не удалось удалить runtime $runtime_basename"
      fi
    done
    rmdir "$runtime_versions" 2>/dev/null || :
  fi
  rmdir "$runtime_root" 2>/dev/null || :
}

macos_shortcut_is_managed() {
  applications_dir=$HOME/Applications
  app_bundle=$applications_dir/Puls.app
  contents=$app_bundle/Contents
  macos_dir=$contents/MacOS
  resources_dir=$contents/Resources
  plist=$contents/Info.plist
  icon_file=$resources_dir/Icon.png
  [ ! -L "$applications_dir" ] && [ -d "$applications_dir" ] && \
    [ ! -L "$app_bundle" ] && [ -d "$app_bundle" ] && \
    [ ! -L "$contents" ] && [ -d "$contents" ] && \
    [ ! -L "$macos_dir" ] && [ -d "$macos_dir" ] && \
    [ ! -L "$resources_dir" ] && [ -d "$resources_dir" ] && \
    [ ! -L "$plist" ] && [ -f "$plist" ] && \
    { [ ! -e "$icon_file" ] || { [ ! -L "$icon_file" ] && [ -f "$icon_file" ]; }; } && \
    grep -Fq '<string>io.github.cheviiot.puls</string>' "$plist" && \
    grep -Fq '<key>PulsInstallerManaged</key><true/>' "$plist"
}

validate_macos_shortcut_target() {
  [ "$install_shortcut" -eq 1 ] || return 0
  [ "$target_os" = darwin ] || return 0
  [ -n "${HOME:-}" ] || fail "не задан HOME; используйте --no-shortcut"
  applications_dir=$HOME/Applications
  if [ -L "$applications_dir" ] || { [ -e "$applications_dir" ] && [ ! -d "$applications_dir" ]; }; then
    fail "$applications_dir уже существует и не является безопасным каталогом"
  fi
  app_bundle=$applications_dir/Puls.app
  if [ ! -e "$app_bundle" ] && [ ! -L "$app_bundle" ]; then
    return 0
  fi
  if ! macos_shortcut_is_managed; then
    fail "$app_bundle уже существует и не принадлежит установщику Puls"
  fi
}

install_linux_shortcut() {
  icon_source=$1
  [ "$install_shortcut" -eq 1 ] || return 0
  [ -n "${HOME:-}" ] || fail "не задан HOME; используйте --no-shortcut"
  data_home=${XDG_DATA_HOME:-$HOME/.local/share}
  applications_dir=$data_home/applications
  icons_dir=$data_home/icons/hicolor/512x512/apps
  mkdir -p "$applications_dir" "$icons_dir"
  desktop_file=$applications_dir/io.github.cheviiot.puls.desktop
  icon_file=$icons_dir/io.github.cheviiot.puls.png
  # Dollar signs and backticks must remain literal in the desktop Exec value.
  # shellcheck disable=SC2016
  escaped_exec=$(printf '%s' "$target_binary" | sed 's/\\/\\\\/g; s/"/\\"/g; s/`/\\`/g; s/\$/\\$/g; s/%/%%/g')
  shortcut_temp=$(mktemp "$applications_dir/.puls-desktop.XXXXXXXX") || \
    fail "не удалось подготовить ярлык Puls"
  {
    printf '%s\n' '[Desktop Entry]'
    printf '%s\n' 'Type=Application'
    printf '%s\n' 'Name=Puls'
    printf '%s\n' 'Comment=Проверка скорости интернета'
    printf 'Exec="%s" gui\n' "$escaped_exec"
    printf '%s\n' 'Icon=io.github.cheviiot.puls'
    printf '%s\n' 'Terminal=false'
    printf '%s\n' 'Categories=Network;Utility;'
    printf '%s\n' 'X-Puls-Managed=true'
  } > "$shortcut_temp" || fail "не удалось записать ярлык Puls"
  chmod 0644 "$shortcut_temp"
  mv -f "$shortcut_temp" "$desktop_file" || fail "не удалось установить ярлык Puls"
  shortcut_temp=""
  cp "$icon_source" "$icon_file" || fail "не удалось установить значок Puls"
  chmod 0644 "$icon_file"
  say "Puls добавлен в меню приложений."
}

install_macos_shortcut() {
  icon_source=$1
  [ "$install_shortcut" -eq 1 ] || return 0
  validate_macos_shortcut_target
  app_bundle=$HOME/Applications/Puls.app
  contents=$app_bundle/Contents
  macos_dir=$contents/MacOS
  resources_dir=$contents/Resources
  mkdir -p "$macos_dir" "$resources_dir"
  # Dollar signs and backticks must remain literal in the launcher path.
  # shellcheck disable=SC2016
  escaped_exec=$(printf '%s' "$target_binary" | sed 's/\\/\\\\/g; s/"/\\"/g; s/`/\\`/g; s/\$/\\$/g')
  shortcut_temp=$(mktemp "$macos_dir/.puls-launcher.XXXXXXXX") || \
    fail "не удалось подготовить приложение Puls"
  printf '#!/bin/sh\nexec "%s" gui "$@"\n' "$escaped_exec" > "$shortcut_temp" || \
    fail "не удалось записать launcher Puls"
  chmod 0755 "$shortcut_temp"
  mv -f "$shortcut_temp" "$macos_dir/Puls" || fail "не удалось установить launcher Puls"
  shortcut_temp=""
  cp "$icon_source" "$resources_dir/Icon.png" || fail "не удалось установить значок Puls"
  cat > "$contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Puls</string>
<key>CFBundleIdentifier</key><string>io.github.cheviiot.puls</string>
<key>CFBundleName</key><string>Puls</string>
<key>CFBundleDisplayName</key><string>Puls</string>
<key>PulsInstallerManaged</key><true/>
<key>CFBundleIconFile</key><string>Icon.png</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSMinimumSystemVersion</key><string>10.15</string>
</dict></plist>
EOF
  say "Puls добавлен в ~/Applications."
}

usage() {
  cat <<'EOF'
Установка и удаление Puls через GitHub Releases

Использование:
  sh install.sh [параметры]

Параметры:
  --version <value>      установить конкретную версию, например 0.3.0
  --install-dir <path>   каталог установки · по умолчанию ~/.local/bin
  --no-path-update       не изменять конфигурацию командной оболочки
  --no-shortcut          не создавать ярлык графического приложения
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
    --no-shortcut)
      install_shortcut=0
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
  if [ -n "${HOME:-}" ]; then
    install_dir=$HOME/.local/bin
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

case $(uname -s) in
  Linux) target_os=linux ;;
  Darwin) target_os=darwin ;;
  *) fail "поддерживаются только Linux и macOS" ;;
esac

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
    if [ -L "$target_binary" ]; then
      case "$(readlink "$target_binary")" in
        .puls-runtime/current/bin/puls|.puls-runtime/current/puls.app/Contents/MacOS/puls) ;;
        *) fail "$target_binary является ссылкой вне управляемого runtime" ;;
      esac
    fi
    rm -f -- "$target_binary" || fail "не удалось удалить $target_binary"
    say "Puls удалён: $target_binary"
  else
    say "Puls уже удалён: $target_binary"
  fi
  remove_managed_runtime
  if [ "$update_path" -eq 1 ]; then
    remove_path_configuration
  fi
  remove_gui_shortcut
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

prepare_path_update

case $(uname -m) in
  x86_64|amd64) target_arch=amd64 ;;
  arm64|aarch64) target_arch=arm64 ;;
  *) fail "неподдерживаемая архитектура $(uname -m)" ;;
esac

if [ -z "$version" ]; then
  manifest_url="$repository_url/releases/latest/download/RELEASE_MANIFEST.json"
else
  version=${version#v}
  manifest_url="$repository_url/releases/download/v${version}/RELEASE_MANIFEST.json"
fi

if [ -n "$version" ]; then
  case "$version" in
    .|..|*[!0-9A-Za-z._-]*) fail "некорректная версия $version" ;;
  esac
fi

temporary_dir=$(mktemp -d "${TMPDIR:-/tmp}/puls-install.XXXXXXXX") || \
  fail "не удалось создать временный каталог"
manifest_path=$temporary_dir/RELEASE_MANIFEST.json
download "$manifest_url" "$manifest_path" || \
  fail "не удалось скачать RELEASE_MANIFEST.json"

manifest_value() {
  manifest_key=$1
  awk -v key="\"${manifest_key}\":" '
    $1 == key {
      value = $2
      sub(/,$/, "", value)
      sub(/^"/, "", value)
      sub(/"$/, "", value)
      print value
      count++
    }
    END { if (count != 1) exit 1 }
  ' "$manifest_path"
}

manifest_schema=$(manifest_value schema_version) || \
  fail "RELEASE_MANIFEST.json имеет некорректную schema"
manifest_product=$(manifest_value product) || \
  fail "RELEASE_MANIFEST.json не содержит product"
manifest_version=$(manifest_value version) || \
  fail "RELEASE_MANIFEST.json не содержит version"
if { [ "$manifest_schema" != 1 ] && [ "$manifest_schema" != 2 ]; } || [ "$manifest_product" != Puls ]; then
  fail "RELEASE_MANIFEST.json имеет неподдерживаемую schema"
fi

case "$manifest_version" in
  ""|.|..|*[!0-9A-Za-z._-]*) \
    fail "RELEASE_MANIFEST.json содержит некорректную version" ;;
esac
if [ -z "$version" ]; then
  version=$manifest_version
elif [ "$manifest_version" != "$version" ]; then
  fail "версия RELEASE_MANIFEST.json не совпадает с запрошенной $version"
fi

if ! manifest_asset=$(awk -v wanted_os="$target_os" -v wanted_arch="$target_arch" -v schema="$manifest_schema" '
  function reset_asset() {
    asset_os = ""
    asset_arch = ""
    asset_file = ""
    asset_sha = ""
    asset_kind = ""
    os_count = arch_count = file_count = sha_count = 0
    kind_count = 0
    asset_cli = 0
    asset_gui = 0
  }
  function string_value(line, value) {
    value = line
    sub(/^[[:space:]]*"[^"]+":[[:space:]]*"/, "", value)
    sub(/",?[[:space:]]*$/, "", value)
    return value
  }
  $1 == "\"assets\":" && $2 == "[" { in_assets = 1; next }
  in_assets && /^[[:space:]]*\{[[:space:]]*$/ {
    if (in_asset) invalid = 1
    in_asset = 1
    reset_asset()
    next
  }
  in_asset && $1 == "\"os\":" { asset_os = string_value($0); os_count++; next }
  in_asset && $1 == "\"arch\":" { asset_arch = string_value($0); arch_count++; next }
  in_asset && $1 == "\"file\":" { asset_file = string_value($0); file_count++; next }
  in_asset && $1 == "\"sha256\":" { asset_sha = string_value($0); sha_count++; next }
  in_asset && $1 == "\"kind\":" { asset_kind = string_value($0); kind_count++; next }
  in_asset && /^[[:space:]]*"cli",?[[:space:]]*$/ { asset_cli = 1; next }
  in_asset && /^[[:space:]]*"gui",?[[:space:]]*$/ { asset_gui = 1; next }
  in_asset && /^[[:space:]]*\},?[[:space:]]*$/ {
    if (os_count != 1 || arch_count != 1 || file_count != 1 || sha_count != 1) invalid = 1
    if (schema == 2 && kind_count != 1) invalid = 1
    if (asset_os == wanted_os && asset_arch == wanted_arch) {
      if (schema == 2 && (asset_kind != "archive" || asset_cli != 1)) invalid = 1
      print asset_file " " asset_sha " " asset_gui
      matches++
    }
    in_asset = 0
    next
  }
  in_assets && !in_asset && /^[[:space:]]*\][[:space:]]*$/ { in_assets = 0 }
  END { if (invalid || in_asset || in_assets || matches != 1) exit 1 }
' "$manifest_path"); then
  fail "в RELEASE_MANIFEST.json нет единственного пакета для ${target_os}/${target_arch}"
fi

asset=${manifest_asset%% *}
manifest_remainder=${manifest_asset#* }
manifest_checksum=${manifest_remainder%% *}
asset_gui=${manifest_remainder##* }
expected_asset="Puls_${version}_${target_os}_${target_arch}.tar.gz"
[ "$asset" = "$expected_asset" ] || \
  fail "RELEASE_MANIFEST.json указывает неожиданный пакет $asset"
case "$manifest_checksum" in
  *[!0-9A-Fa-f]*) fail "RELEASE_MANIFEST.json содержит некорректный SHA-256" ;;
esac
[ "${#manifest_checksum}" -eq 64 ] || \
  fail "RELEASE_MANIFEST.json содержит некорректный SHA-256"
if [ "$asset_gui" -eq 1 ]; then
  validate_macos_shortcut_target
fi

release_url="$repository_url/releases/download/v${version}"
archive_path=$temporary_dir/$asset
checksums_path=$temporary_dir/SHA256SUMS.txt
say "Загрузка Puls ${version} для ${target_os}/${target_arch}..."
download "$release_url/$asset" "$archive_path" || fail "не удалось скачать $asset"
download "$release_url/SHA256SUMS.txt" "$checksums_path" || \
  fail "не удалось скачать SHA256SUMS.txt"

checksum_for() {
  checksum_name=$1
  awk -v name="$checksum_name" '
    $2 == name && NF == 2 { print $1; count++ }
    END { if (count != 1) exit 1 }
  ' "$checksums_path"
}

expected_checksum=$(checksum_for "$asset") || \
  fail "в SHA256SUMS.txt нет единственной записи для $asset"
expected_manifest_checksum=$(checksum_for RELEASE_MANIFEST.json) || \
  fail "в SHA256SUMS.txt нет единственной записи для RELEASE_MANIFEST.json"
for listed_checksum in "$expected_checksum" "$expected_manifest_checksum"; do
  case "$listed_checksum" in
    *[!0-9A-Fa-f]*) fail "в SHA256SUMS.txt указан некорректный SHA-256" ;;
  esac
  [ "${#listed_checksum}" -eq 64 ] || \
    fail "в SHA256SUMS.txt указан некорректный SHA-256"
done

if command -v sha256sum >/dev/null 2>&1; then
  actual_checksum=$(sha256sum "$archive_path" | awk '{ print $1 }')
  actual_manifest_checksum=$(sha256sum "$manifest_path" | awk '{ print $1 }')
elif command -v shasum >/dev/null 2>&1; then
  actual_checksum=$(shasum -a 256 "$archive_path" | awk '{ print $1 }')
  actual_manifest_checksum=$(shasum -a 256 "$manifest_path" | awk '{ print $1 }')
else
  fail "не найден sha256sum или shasum"
fi
expected_checksum=$(printf '%s' "$expected_checksum" | tr '[:upper:]' '[:lower:]')
expected_manifest_checksum=$(printf '%s' "$expected_manifest_checksum" | tr '[:upper:]' '[:lower:]')
manifest_checksum=$(printf '%s' "$manifest_checksum" | tr '[:upper:]' '[:lower:]')
actual_checksum=$(printf '%s' "$actual_checksum" | tr '[:upper:]' '[:lower:]')
actual_manifest_checksum=$(printf '%s' "$actual_manifest_checksum" | tr '[:upper:]' '[:lower:]')
[ "$actual_manifest_checksum" = "$expected_manifest_checksum" ] || \
  fail "контрольная сумма RELEASE_MANIFEST.json не совпала"
[ "$manifest_checksum" = "$expected_checksum" ] || \
  fail "SHA-256 пакета различается в manifest и SHA256SUMS.txt"
[ "$actual_checksum" = "$expected_checksum" ] || fail "контрольная сумма архива не совпала"

extract_dir=$temporary_dir/extracted
mkdir -p "$extract_dir"
package_dir=${asset%.tar.gz}
binary_member=$package_dir/puls
icon_path=""
runtime_member=""
if tar -tzf "$archive_path" | grep -Fqx "$package_dir/bin/puls"; then
  runtime_member=bin/puls
elif tar -tzf "$archive_path" | grep -Fqx "$package_dir/puls.app/Contents/MacOS/puls"; then
  runtime_member=puls.app/Contents/MacOS/puls
fi

if [ -n "$runtime_member" ]; then
  validate_archive_members "$archive_path" "$package_dir"
  validate_archive_links "$archive_path" "$package_dir"
  tar -xzf "$archive_path" -C "$extract_dir" --no-same-owner || \
    fail "не удалось извлечь runtime из пакета"
  runtime_tree=$extract_dir/$package_dir
  binary_path=$runtime_tree/$runtime_member
  if [ ! -f "$binary_path" ] || [ -L "$binary_path" ]; then
    fail "в runtime не найден обычный файл puls"
  fi
  case "$runtime_member" in
    bin/puls) [ "$target_os" = linux ] || fail "runtime имеет неожиданную структуру для $target_os" ;;
    puls.app/*) [ "$target_os" = darwin ] || fail "runtime имеет неожиданную структуру для $target_os" ;;
  esac
  icon_path=$runtime_tree/assets/Icon.png
  [ -f "$icon_path" ] && [ ! -L "$icon_path" ] || icon_path=""
  if [ "$asset_gui" -eq 1 ] && [ -z "$icon_path" ]; then
    fail "в runtime не найден assets/Icon.png для ярлыка"
  fi
  find "$runtime_tree" -type l -print | while IFS= read -r runtime_link; do
    link_target=$(readlink "$runtime_link") || fail "не удалось проверить ссылку runtime"
    case "$link_target" in
      /*|*\\*) fail "runtime содержит небезопасную ссылку" ;;
    esac
    link_relative=${runtime_link#"$runtime_tree"/}
    if ! printf '%s\n%s\n' "${link_relative%/*}" "$link_target" | awk '
      NR == 1 { count = split($0, base, "/"); for (i = 1; i <= count; i++) if (base[i] != "" && base[i] != ".") stack[++depth] = base[i]; next }
      NR == 2 { count = split($0, target, "/"); for (i = 1; i <= count; i++) { if (target[i] == "" || target[i] == ".") continue; if (target[i] == "..") { if (depth == 0) exit 1; depth-- } else stack[++depth] = target[i] } }
      END { if (NR != 2) exit 1 }
    '; then
      fail "runtime содержит ссылку за пределы каталога"
    fi
  done || fail "runtime содержит некорректную ссылку"
  install_runtime_tree "$runtime_tree" "$runtime_member"
else
  tar -xzf "$archive_path" -C "$extract_dir" "$binary_member" || \
    fail "не удалось извлечь puls из пакета"
  binary_path=$extract_dir/$binary_member
  if [ ! -f "$binary_path" ] || [ -L "$binary_path" ]; then
    fail "в архиве не найден обычный файл puls"
  fi
  if [ "$asset_gui" -eq 1 ]; then
    icon_member=$package_dir/assets/Icon.png
    tar -xzf "$archive_path" -C "$extract_dir" "$icon_member" || \
      fail "не удалось извлечь значок Puls из пакета"
    icon_path=$extract_dir/$icon_member
    if [ ! -f "$icon_path" ] || [ -L "$icon_path" ]; then
      fail "в архиве не найден обычный файл Icon.png"
    fi
  fi
fi

if [ -z "$runtime_member" ]; then
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
else
  install_action=установлен
  if [ -L "$install_dir/puls" ] && runtime_pointer_target "$install_dir/.puls-runtime/current"; then
    install_action=обновлён
  fi
fi

say "Puls $version $install_action: $target_binary"
add_path_configuration
if [ "$asset_gui" -eq 1 ]; then
  case "$target_os" in
    linux) install_linux_shortcut "$icon_path" ;;
    darwin) install_macos_shortcut "$icon_path" ;;
  esac
fi
