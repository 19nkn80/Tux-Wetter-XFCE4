# TuX-Wetter 0.2 for Xfce 4

Native Xfce panel-plugin port of TuX-Wetter. It uses Open-Meteo without an
account or API key; there are no paid or key-based provider options.
The presentation follows the ideas in the
[TuX-Wetter forum thread](https://forum.linuxguides.de/index.php?thread/17421-tux-wetter-v1-3-neue-version-25-10-26/).

## Build and install

Dependencies: Xfce Panel 4, GTK 3, libsoup 3, JSON-GLib, a C compiler and
`pkg-config`. Distribution-specific commands are in [README.md](README.md).
`notify-send` is optional and is needed only for desktop warning notifications.

```sh
make
sudo make install
```

Then add **TuX-Wetter** through *Panel → Add New Items*. `make install
PREFIX="$HOME/.local" LIBDIR="$HOME/.local/lib"` installs under `~/.local`
without administrator privileges. Restart the panel or log out and back in if
it is already running.
For a user-local install and safe panel restart, run `./install-user.sh`.
It installs only the plugin binary and desktop entry; it never replaces
personal settings. If no panel process is running, it avoids the failed
`xfce4-panel -r` D-Bus restart and asks you to restart the XFCE session.

The widget provides the panel readout, Open-Meteo forecast and geocoding,
saved places, a six-hour strip, seven-day forecast, clickable daily temperature
charts, Celsius/Fahrenheit, refresh interval, color themes, and settings to
show or hide panel elements and details. MET Norway and DWD/Bright Sky can be
selected for current conditions; the forecast, UV and sunrise/sunset remain
from Open-Meteo. Air-quality index and six pollen types come from Open-Meteo;
moon phase and rise/set come from MET Norway. Public weather warnings are
queried for Switzerland (MeteoAlarm), Austria (GeoSphere) and Germany (DWD).
Desktop warning notifications have an adjustable minimum severity. DWD station
observations are shown when selecting DWD and a nearby reading is recent enough.

Automatic IP-based location is optional and disabled by default; enabling it
sends the public IP-derived location request to GeoJS. Preferences offer
automatic, German and English display language choices, and an optional gentle
weather-icon animation while the popup is open. Settings can be exported to or
imported from an INI backup. Data is stored separately from the plugin in
`~/.config/tux-wetter-xfce/settings.ini`. Settings are written as they change,
with the previous file kept as `settings.ini.bak`; a damaged primary file is
restored from that backup on startup. Updating or reinstalling the plugin does
not overwrite either settings file.

The arrows beside the place name cycle through saved places and the gear opens
the preferences. The plugin checks the public GitHub API at most once a day
for a newer version (no login, can be disabled in preferences) and offers an
Update/Cancel dialog; Update runs `update-user.sh` in a terminal, which
downloads, builds and installs the new version and reloads the panel.

This is a native GTK/Xfce UI, not a way to load the Plasma `.plasmoid` in Xfce.
Some Plasma behavior is still not ported: MET Norway and DWD do not yet provide
their own forecast timelines; measured readings from MeteoSchweiz and
GeoSphere are not included; warnings are not yet filtered by individual hazard
types; the English translation does not cover every settings label; and the
animated SVG weather scenes and 15-minute rain nowcast are not implemented.
No paid or API-key-based provider is offered.

This software is licensed under GPL-3.0-or-later; see [COPYING](COPYING).
