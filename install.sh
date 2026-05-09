#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="${repo_dir}/build"
prefix="/usr/local"
binary="${prefix}/bin/kwinshot"
desktop_file="${prefix}/share/applications/net.local.kwinshot.desktop"
desktop_basename="net.local.kwinshot.desktop"

if ! command -v cmake >/dev/null 2>&1; then
    echo "install.sh: cmake is required" >&2
    exit 1
fi

desktop_home="${HOME}"
if (( EUID == 0 )) && [[ -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]]; then
    desktop_home="$(getent passwd "${SUDO_USER}" | cut -d: -f6)"
fi
user_desktop_file="${XDG_DATA_HOME:-${desktop_home}/.local/share}/applications/${desktop_basename}"
if [[ -e "${user_desktop_file}" ]]; then
    echo "install.sh: warning: ${user_desktop_file} may shadow the trusted system desktop file" >&2
    echo "install.sh: remove or rename it if KDE shortcuts or screenshot authorization fail" >&2
fi

if (( EUID == 0 )); then
    sudo_cmd=()
else
    if ! command -v sudo >/dev/null 2>&1; then
        echo "install.sh: sudo is required when not running as root" >&2
        exit 1
    fi
    sudo_cmd=(sudo)
fi

echo "==> Configuring"
cmake -S "${repo_dir}" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Release

echo "==> Building"
cmake --build "${build_dir}"

if command -v desktop-file-validate >/dev/null 2>&1; then
    echo "==> Validating desktop file"
    desktop-file-validate "${repo_dir}/data/net.local.kwinshot.desktop"
fi

echo "==> Installing to ${prefix}"
"${sudo_cmd[@]}" cmake --install "${build_dir}" --prefix "${prefix}"

echo "==> Fixing ownership and permissions for KWin restricted screenshot API"
"${sudo_cmd[@]}" chown root:root "${binary}" "${desktop_file}"
"${sudo_cmd[@]}" chmod 755 "${binary}"
"${sudo_cmd[@]}" chmod 644 "${desktop_file}"

if command -v kbuildsycoca6 >/dev/null 2>&1; then
    echo "==> Rebuilding KDE service cache"
    if (( EUID == 0 )) && [[ -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]] && command -v sudo >/dev/null 2>&1; then
        user_id="$(id -u "${SUDO_USER}")"
        sudo -u "${SUDO_USER}" env XDG_RUNTIME_DIR="/run/user/${user_id}" kbuildsycoca6 --noincremental
    else
        kbuildsycoca6 --noincremental
    fi
else
    echo "install.sh: kbuildsycoca6 not found; log out/in if KDE does not see KWinShot yet" >&2
fi

echo
echo "Installed kwinshot."
echo "Bind the installed KWinShot actions in KDE shortcut settings:"
echo "  - Capture Rectangular Region"
echo "  - Capture Active Window"
echo "  - Capture Current Screen"
echo "  - Capture Region to Clipboard"
