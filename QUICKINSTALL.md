# TuX-Wetter XFCE 0.1 — Schnellinstallation

Diese Anleitung installiert das Plugin für den aktuellen Benutzer. Das Wetter
wird über kostenlose, öffentliche Dienste geladen; ein API-Schlüssel ist nicht
erforderlich. Eine Internetverbindung wird benötigt.

## 1. Abhängigkeiten installieren

Öffne ein Terminal und führe **nur den Befehl für deine Distribution** aus.
Das Passwort ist dein normales Linux-Passwort; bei der Eingabe wird nichts
angezeigt.

### Arch Linux / Manjaro

```sh
sudo pacman -S --needed base-devel pkgconf xfce4-panel gtk3 libsoup3 json-glib
```

### Debian / Ubuntu / Linux Mint

```sh
sudo apt update
sudo apt install build-essential pkg-config xfce4-panel libxfce4panel-2.0-dev libgtk-3-dev libsoup-3.0-dev libjson-glib-dev
```

### Fedora

```sh
sudo dnf install gcc make pkgconf-pkg-config xfce4-panel-devel gtk3-devel libsoup3-devel json-glib-devel
```

Für optionale Desktop-Benachrichtigungen bei Warnungen zusätzlich installieren:

- Arch / Manjaro: `libnotify`
- Debian / Ubuntu / Mint: `libnotify-bin`
- Fedora: `libnotify`

## 2. Paket entpacken

Wechsle im Dateimanager in den Ordner, in dem du
`xfce4-tux-wetter-0.1.tar.gz` gespeichert hast. Rechtsklick auf das Archiv →
**Hier entpacken**. Öffne anschließend den entpackten Ordner
`xfce4-tux-wetter-0.1` und dort ein Terminal.

## 3. Installieren

Führe im Terminal des entpackten Ordners aus:

```sh
./install-user.sh
```

Das Skript baut das Plugin, installiert es nur für deinen Benutzer und lädt
das XFCE-Panel neu. Deine persönlichen Einstellungen werden dabei nicht
überschrieben.

## 4. Wetter-Widget zum Panel hinzufügen

Rechtsklick auf ein XFCE-Panel → **Panel → Neue Elemente hinzufügen** →
**TuX-Wetter** → **Hinzufügen**. Danach erscheint das Wetter-Widget im Panel.

## Einstellungen und Fehlerhilfe

Die Einstellungen bleiben bei Updates erhalten und liegen unter
`~/.config/tux-wetter-xfce/`.

Falls die Installation mit einer fehlenden Abhängigkeit abbricht, installiere
die Pakete aus Schritt 1 für deine Distribution und führe
`./install-user.sh` erneut aus. Ein Selbsttest ist optional:

```sh
make test
```
