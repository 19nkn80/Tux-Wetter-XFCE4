#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
data_home=${XDG_DATA_HOME:-"$HOME/.local/share"}
plugin_dir="$HOME/.local/lib/xfce4/panel/plugins"
desktop_dir="$data_home/xfce4/panel/plugins"

make -C "$script_dir" all
install -Dm755 "$script_dir/libxfce4-tux-wetter.so" \
    "$plugin_dir/libxfce4-tux-wetter.so"
install -Dm644 "$script_dir/xfce4-tux-wetter.desktop" \
    "$desktop_dir/xfce4-tux-wetter.desktop"

if pgrep -u "$(id -u)" -x xfce4-panel >/dev/null 2>&1; then
    xfce4-panel -r
else
    printf '%s\n' \
        'TuX-Wetter ist installiert. Das XFCE-Panel läuft gerade nicht.' \
        'Starte es über deine Sitzung neu oder melde dich ab und wieder an.'
fi

printf '%s\n' \
    'Die persönlichen Einstellungen unter ~/.config/tux-wetter-xfce/' \
    'wurden nicht geändert.'
