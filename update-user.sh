#!/bin/sh
# Lädt die angegebene (oder neueste) Version von GitHub und installiert sie
# für den aktuellen Benutzer. Aufruf: update-user.sh [TAG]
set -eu

repo="19nkn80/Tux-Wetter-XFCE4"
tag=${1:-}

for tool in curl tar make; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf 'Es fehlt das Programm "%s". Siehe README.md (Abhängigkeiten).\n' "$tool"
        exit 1
    }
done

if [ -z "$tag" ]; then
    tag=$(curl -fsSL "https://api.github.com/repos/$repo/tags" |
        sed -n 's/.*"name": *"\([^"]*\)".*/\1/p' | sort -V | tail -n 1)
fi
case "$tag" in
    ''|*[!A-Za-z0-9._-]*)
        printf 'Keine gültige Version gefunden.\n'
        exit 1
        ;;
esac

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

printf 'Lade TuX-Wetter %s …\n' "$tag"
curl -fsSL "https://github.com/$repo/archive/refs/tags/$tag.tar.gz" |
    tar -xz -C "$work" --strip-components=1

sh "$work/install-user.sh"
printf '\nUpdate auf %s abgeschlossen.\n' "$tag"
