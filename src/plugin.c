/*
 * SPDX-FileCopyrightText: 2026 R.Bruns
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "weather.h"

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <libxfce4panel/libxfce4panel.h>
#include <libsoup/soup.h>
#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define APP_NAME "TuX-Wetter"
#define APP_VERSION "0.1"
#define APP_USER_AGENT "TuX-Wetter-Xfce/" APP_VERSION
#define UPDATE_DEFAULT 15
#define PLACES_KEY "places"

typedef struct
{
    gchar *name;
    gdouble latitude;
    gdouble longitude;
} Place;

typedef struct
{
    XfcePanelPlugin *plugin;
    GtkWidget *button;
    GtkWidget *panel_box;
    GtkWidget *panel_image;
    GtkWidget *panel_temperature;
    GtkWidget *weather_window;
    GtkWidget *place_name;
    GtkWidget *current_image;
    GtkWidget *temperature;
    GtkWidget *summary;
    GtkWidget *details;
    GtkWidget *warning;
    GtkWidget *hours_title;
    GtkWidget *hours_box;
    GtkWidget *info;
    GtkWidget *days_box;
    GtkWidget *days_title;
    GtkWidget *source;
    GtkWidget *status;
    GtkWidget *extras;
    SoupSession *session;
    GKeyFile *settings;
    gchar *settings_path;
    GPtrArray *places;
    Place *auto_place;
    WeatherData *weather;
    guint place_index;
    gint selected_day;
    guint refresh_minutes;
    guint request_serial;
    guint refresh_source;
    guint animation_source;
    gchar *provider;
    gchar *language;
    gchar *official_warning;
    gchar *last_notification;
    gboolean fahrenheit;
    gboolean show_panel_icon;
    gboolean show_panel_temperature;
    gboolean show_details;
    gboolean show_forecast;
    gboolean show_air_quality;
    gboolean show_moon;
    gboolean show_measured;
    gboolean animations;
    gboolean auto_location;
    gboolean warnings_enabled;
    gboolean notifications_enabled;
    guint notification_level;
    gchar *theme;
} WeatherApplet;

typedef struct
{
    WeatherApplet *applet;
    GtkWidget *results;
    GtkWidget *status;
    SoupMessage *message;
} SearchContext;

typedef struct
{
    XfcePanelPlugin *plugin;
    SoupMessage *message;
    guint serial;
} WeatherRequest;

typedef struct
{
    XfcePanelPlugin *plugin;
    SoupMessage *message;
    guint serial;
    gboolean metno;
} ProviderRequest;

typedef struct
{
    XfcePanelPlugin *plugin;
    SoupMessage *message;
    guint serial;
    gboolean moon;
} AuxiliaryRequest;

typedef struct
{
    XfcePanelPlugin *plugin;
    SoupMessage *message;
} AutoLocationRequest;

typedef struct
{
    XfcePanelPlugin *plugin;
    SoupMessage *message;
    guint serial;
    gint country;
    gdouble latitude;
    gdouble longitude;
} AlertRequest;

static void fetch_weather(WeatherApplet *applet);
static void update_display(WeatherApplet *applet);
static void show_preferences(WeatherApplet *applet);
static void toggle_weather_window(GtkWidget *widget, gpointer user_data);
static gboolean hide_weather_window(GtkWidget *widget,
                                    GdkEvent *event,
                                    gpointer user_data);
static void save_settings(WeatherApplet *applet);
static gboolean refresh_timeout(gpointer user_data);
static void fetch_provider_current(WeatherApplet *applet);
static void fetch_auxiliary(WeatherApplet *applet);
static void fetch_alerts(WeatherApplet *applet);
static void fetch_auto_location(WeatherApplet *applet);
static void set_animation_state(WeatherApplet *applet);

static void
place_free(gpointer data)
{
    Place *place = data;
    if (!place)
        return;
    g_free(place->name);
    g_free(place);
}

static gboolean
use_english(WeatherApplet *applet)
{
    const gchar *const *languages;

    if (g_strcmp0(applet->language, "en") == 0)
        return TRUE;
    if (g_strcmp0(applet->language, "de") == 0)
        return FALSE;
    languages = g_get_language_names();
    return languages && languages[0] &&
        g_str_has_prefix(languages[0], "en");
}

static Place *
place_new(const gchar *name,
          gdouble latitude,
          gdouble longitude)
{
    Place *place = g_new0(Place, 1);
    place->name = g_strdup(name);
    place->latitude = latitude;
    place->longitude = longitude;
    return place;
}

static Place *
current_place(WeatherApplet *applet)
{
    if (applet->auto_location && applet->auto_place)
        return applet->auto_place;
    if (!applet->places->len)
        return NULL;
    return g_ptr_array_index(applet->places,
                             applet->place_index % applet->places->len);
}

static WeatherApplet *
get_applet(XfcePanelPlugin *plugin)
{
    return g_object_get_data(G_OBJECT(plugin), "tux-weather-applet");
}

static void
load_settings(WeatherApplet *applet)
{
    GError *error = NULL;
    GError *backup_error = NULL;
    gchar **saved_places = NULL;
    gchar *backup_path;
    gsize n_places = 0;
    gsize i;
    gint saved_place_index = 0;
    gboolean loaded;

    applet->settings = g_key_file_new();
    applet->places = g_ptr_array_new_with_free_func(place_free);
    applet->refresh_minutes = UPDATE_DEFAULT;
    applet->theme = g_strdup("violett");
    applet->provider = g_strdup("openmeteo");
    applet->language = g_strdup("auto");
    applet->show_panel_icon = TRUE;
    applet->show_panel_temperature = TRUE;
    applet->show_details = TRUE;
    applet->show_forecast = TRUE;
    applet->show_air_quality = TRUE;
    applet->show_moon = TRUE;
    applet->show_measured = TRUE;
    applet->animations = TRUE;
    applet->auto_location = FALSE;
    applet->warnings_enabled = TRUE;
    applet->notifications_enabled = TRUE;
    applet->notification_level = 2;
    applet->settings_path = g_build_filename(g_get_user_config_dir(),
                                             "tux-wetter-xfce",
                                             "settings.ini", NULL);

    loaded = g_key_file_load_from_file(applet->settings, applet->settings_path,
                                       G_KEY_FILE_NONE, &error);
    if (!loaded && !g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT))
        g_warning("%s: cannot read settings: %s", APP_NAME, error->message);
    g_clear_error(&error);

    backup_path = g_strconcat(applet->settings_path, ".bak", NULL);
    if (!loaded)
    {
        loaded = g_key_file_load_from_file(applet->settings, backup_path,
                                           G_KEY_FILE_NONE, &backup_error);
        if (loaded)
            g_warning("%s: restored settings from backup %s",
                      APP_NAME, backup_path);
        else if (!g_error_matches(backup_error, G_FILE_ERROR,
                                  G_FILE_ERROR_NOENT))
            g_warning("%s: cannot read settings backup: %s",
                      APP_NAME, backup_error->message);
    }
    g_clear_error(&backup_error);
    g_free(backup_path);

    if (loaded)
    {
        applet->fahrenheit = g_key_file_get_boolean(applet->settings, "display",
                                                    "fahrenheit", NULL);
        applet->refresh_minutes = (guint) g_key_file_get_integer(applet->settings,
                                                                  "display",
                                                                  "refresh-minutes",
                                                                  NULL);
        g_free(applet->theme);
        applet->theme = g_key_file_get_string(applet->settings, "display",
                                              "theme", NULL);
        g_free(applet->provider);
        applet->provider = g_key_file_get_string(applet->settings, "provider",
                                                  "weather-source", NULL);
        g_free(applet->language);
        applet->language = g_key_file_get_string(applet->settings, "display",
                                                  "language", NULL);
        if (g_key_file_has_key(applet->settings, "display",
                               "show-panel-icon", NULL))
            applet->show_panel_icon = g_key_file_get_boolean(
                applet->settings, "display", "show-panel-icon", NULL);
        if (g_key_file_has_key(applet->settings, "display",
                               "show-panel-temperature", NULL))
            applet->show_panel_temperature = g_key_file_get_boolean(
                applet->settings, "display", "show-panel-temperature", NULL);
        if (g_key_file_has_key(applet->settings, "display",
                               "show-details", NULL))
            applet->show_details = g_key_file_get_boolean(
                applet->settings, "display", "show-details", NULL);
        if (g_key_file_has_key(applet->settings, "display",
                               "show-forecast", NULL))
            applet->show_forecast = g_key_file_get_boolean(
                applet->settings, "display", "show-forecast", NULL);
        if (g_key_file_has_key(applet->settings, "display",
                               "show-air-quality", NULL))
            applet->show_air_quality = g_key_file_get_boolean(
                applet->settings, "display", "show-air-quality", NULL);
        if (g_key_file_has_key(applet->settings, "display",
                               "show-moon", NULL))
            applet->show_moon = g_key_file_get_boolean(
                applet->settings, "display", "show-moon", NULL);
        if (g_key_file_has_key(applet->settings, "display",
                               "show-measured", NULL))
            applet->show_measured = g_key_file_get_boolean(
                applet->settings, "display", "show-measured", NULL);
        if (g_key_file_has_key(applet->settings, "display",
                               "animations", NULL))
            applet->animations = g_key_file_get_boolean(
                applet->settings, "display", "animations", NULL);
        saved_places = g_key_file_get_string_list(applet->settings, "location",
                                                   PLACES_KEY, &n_places, NULL);
        saved_place_index = g_key_file_get_integer(applet->settings, "location",
                                                  "active-place-index", NULL);
        applet->auto_location = g_key_file_get_boolean(applet->settings,
                                            "location", "auto-enabled", NULL);
        if (g_key_file_has_key(applet->settings, "alerts", "enabled", NULL))
            applet->warnings_enabled = g_key_file_get_boolean(
                applet->settings, "alerts", "enabled", NULL);
        if (g_key_file_has_key(applet->settings, "alerts", "notifications", NULL))
            applet->notifications_enabled = g_key_file_get_boolean(
                applet->settings, "alerts", "notifications", NULL);
        if (g_key_file_has_key(applet->settings, "alerts", "minimum-level", NULL))
            applet->notification_level = (guint) g_key_file_get_integer(
                applet->settings, "alerts", "minimum-level", NULL);
        if (g_key_file_has_key(applet->settings, "location", "auto-name", NULL) &&
            g_key_file_has_key(applet->settings, "location", "auto-latitude", NULL) &&
            g_key_file_has_key(applet->settings, "location", "auto-longitude", NULL))
        {
            gchar *name = g_key_file_get_string(applet->settings, "location",
                                                "auto-name", NULL);
            gdouble lat = g_key_file_get_double(applet->settings, "location",
                                                "auto-latitude", NULL);
            gdouble lon = g_key_file_get_double(applet->settings, "location",
                                                "auto-longitude", NULL);
            if (name && isfinite(lat) && isfinite(lon) &&
                lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)
                applet->auto_place = place_new(name, lat, lon);
            g_free(name);
        }
    }

    if (saved_places)
    {
        for (i = 0; i < n_places; i++)
        {
            gchar **fields = g_strsplit(saved_places[i], "|", 3);
            if (fields[0] && fields[1] && fields[2])
            {
                gchar *name = g_uri_unescape_string(fields[0], NULL);
                gchar *end_lat = NULL;
                gchar *end_lon = NULL;
                gdouble lat = g_ascii_strtod(fields[1], &end_lat);
                gdouble lon = g_ascii_strtod(fields[2], &end_lon);
                if (end_lat != fields[1] && *end_lat == '\0' &&
                    end_lon != fields[2] && *end_lon == '\0' &&
                    isfinite(lat) && isfinite(lon) &&
                    lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)
                    g_ptr_array_add(applet->places,
                                    place_new(name ? name : fields[0], lat, lon));
                g_free(name);
            }
            g_strfreev(fields);
        }
        g_strfreev(saved_places);
    }

    if (applet->places->len == 0)
        g_ptr_array_add(applet->places, place_new("Luzern", 47.0502, 8.3093));
    if (saved_place_index >= 0 &&
        (guint) saved_place_index < applet->places->len)
        applet->place_index = (guint) saved_place_index;
    if (applet->refresh_minutes < 10 || applet->refresh_minutes > 180)
        applet->refresh_minutes = UPDATE_DEFAULT;
    if (applet->notification_level < 1 || applet->notification_level > 4)
        applet->notification_level = 2;
    if (!applet->theme || !*applet->theme)
    {
        g_free(applet->theme);
        applet->theme = g_strdup("violett");
    }
    if (!applet->provider ||
        (g_strcmp0(applet->provider, "openmeteo") != 0 &&
         g_strcmp0(applet->provider, "metno") != 0 &&
         g_strcmp0(applet->provider, "dwd") != 0))
    {
        g_free(applet->provider);
        applet->provider = g_strdup("openmeteo");
    }
    if (!applet->language ||
        (g_strcmp0(applet->language, "auto") != 0 &&
         g_strcmp0(applet->language, "de") != 0 &&
         g_strcmp0(applet->language, "en") != 0))
    {
        g_free(applet->language);
        applet->language = g_strdup("auto");
    }
}

static void
save_settings(WeatherApplet *applet)
{
    gchar **list = g_new0(gchar *, applet->places->len + 1);
    gchar *directory = g_path_get_dirname(applet->settings_path);
    gchar *backup_path = g_strconcat(applet->settings_path, ".bak", NULL);
    GError *error = NULL;
    GError *backup_error = NULL;
    guint i;

    for (i = 0; i < applet->places->len; i++)
    {
        Place *place = g_ptr_array_index(applet->places, i);
        gchar latitude[G_ASCII_DTOSTR_BUF_SIZE];
        gchar longitude[G_ASCII_DTOSTR_BUF_SIZE];
        gchar *name = g_uri_escape_string(place->name, NULL, TRUE);
        g_ascii_dtostr(latitude, sizeof(latitude), place->latitude);
        g_ascii_dtostr(longitude, sizeof(longitude), place->longitude);
        list[i] = g_strdup_printf("%s|%s|%s", name, latitude, longitude);
        g_free(name);
    }
    g_key_file_set_string_list(applet->settings, "location", PLACES_KEY,
                               (const gchar * const *) list, applet->places->len);
    g_key_file_set_integer(applet->settings, "location", "active-place-index",
                           (gint) applet->place_index);
    g_key_file_set_boolean(applet->settings, "location", "auto-enabled",
                           applet->auto_location);
    g_key_file_set_boolean(applet->settings, "alerts", "enabled",
                           applet->warnings_enabled);
    g_key_file_set_boolean(applet->settings, "alerts", "notifications",
                           applet->notifications_enabled);
    g_key_file_set_integer(applet->settings, "alerts", "minimum-level",
                           (gint) applet->notification_level);
    if (applet->auto_place)
    {
        g_key_file_set_string(applet->settings, "location", "auto-name",
                              applet->auto_place->name);
        g_key_file_set_double(applet->settings, "location", "auto-latitude",
                              applet->auto_place->latitude);
        g_key_file_set_double(applet->settings, "location", "auto-longitude",
                              applet->auto_place->longitude);
    }
    g_key_file_set_boolean(applet->settings, "display", "fahrenheit",
                           applet->fahrenheit);
    g_key_file_set_integer(applet->settings, "display", "refresh-minutes",
                           (gint) applet->refresh_minutes);
    g_key_file_set_string(applet->settings, "display", "theme", applet->theme);
    g_key_file_set_string(applet->settings, "provider", "weather-source",
                          applet->provider);
    g_key_file_set_string(applet->settings, "display", "language",
                          applet->language);
    g_key_file_set_boolean(applet->settings, "display", "show-panel-icon",
                           applet->show_panel_icon);
    g_key_file_set_boolean(applet->settings, "display",
                           "show-panel-temperature",
                           applet->show_panel_temperature);
    g_key_file_set_boolean(applet->settings, "display", "show-details",
                           applet->show_details);
    g_key_file_set_boolean(applet->settings, "display", "show-forecast",
                           applet->show_forecast);
    g_key_file_set_boolean(applet->settings, "display", "show-air-quality",
                           applet->show_air_quality);
    g_key_file_set_boolean(applet->settings, "display", "show-moon",
                           applet->show_moon);
    g_key_file_set_boolean(applet->settings, "display", "show-measured",
                           applet->show_measured);
    g_key_file_set_boolean(applet->settings, "display", "animations",
                           applet->animations);

    if (g_mkdir_with_parents(directory, 0700) != 0)
    {
        g_warning("%s: cannot create settings directory: %s",
                  APP_NAME, g_strerror(errno));
    }
    else
    {
        if (g_file_test(applet->settings_path, G_FILE_TEST_IS_REGULAR))
        {
            GFile *source_file = g_file_new_for_path(applet->settings_path);
            GFile *backup_file = g_file_new_for_path(backup_path);
            if (!g_file_copy(source_file, backup_file, G_FILE_COPY_OVERWRITE,
                             NULL, NULL, NULL, &backup_error))
                g_warning("%s: cannot back up settings: %s",
                          APP_NAME, backup_error->message);
            else if (g_chmod(backup_path, 0600) != 0)
                g_warning("%s: cannot secure settings backup: %s",
                          APP_NAME, g_strerror(errno));
            g_clear_error(&backup_error);
            g_object_unref(source_file);
            g_object_unref(backup_file);
        }

        if (!g_key_file_save_to_file(applet->settings,
                                     applet->settings_path, &error))
            g_warning("%s: cannot save settings: %s", APP_NAME, error->message);
        else if (g_chmod(applet->settings_path, 0600) != 0)
            g_warning("%s: cannot secure settings file: %s",
                      APP_NAME, g_strerror(errno));
        g_clear_error(&error);
    }

    g_strfreev(list);
    g_free(backup_path);
    g_free(directory);
}

static gchar *
format_temperature(WeatherApplet *applet,
                  gdouble celsius)
{
    gint value = (gint) lround(applet->fahrenheit
                               ? celsius * 9.0 / 5.0 + 32.0
                               : celsius);
    return g_strdup_printf("%d°%s", value, applet->fahrenheit ? "F" : "C");
}

static GtkWidget *
make_label(const gchar *text,
           const gchar *class_name)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    if (class_name)
        gtk_style_context_add_class(gtk_widget_get_style_context(label), class_name);
    return label;
}

static GtkWidget *
make_icon(const gchar *name,
          gint size)
{
    GtkWidget *image = gtk_image_new_from_icon_name(name, GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(image), size);
    return image;
}

static void
clear_box(GtkWidget *box)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    GList *item;

    for (item = children; item; item = item->next)
        gtk_widget_destroy(GTK_WIDGET(item->data));
    g_list_free(children);
}

static gchar *
format_short_time(const gchar *iso_time)
{
    if (!iso_time || strlen(iso_time) < 16)
        return g_strdup("--:--");
    return g_strndup(iso_time + 11, 5);
}

static gchar *
format_short_date(const gchar *iso_date)
{
    GDateTime *date;
    gchar *formatted;
    gint year;
    gint month;
    gint day;

    if (!iso_date || strlen(iso_date) < 10)
        return g_strdup("—");
    if (sscanf(iso_date, "%4d-%2d-%2d", &year, &month, &day) != 3)
        return g_strdup(iso_date);
    date = g_date_time_new_local(year, month, day, 12, 0, 0);
    if (!date)
        return g_strdup(iso_date);
    formatted = g_date_time_format(date, "%a");
    g_date_time_unref(date);
    return formatted;
}

static gdouble
json_number(JsonObject *object,
            const gchar *name,
            gdouble fallback)
{
    JsonNode *node;
    GType type;

    if (!object || !json_object_has_member(object, name))
        return fallback;
    node = json_object_get_member(object, name);
    if (!JSON_NODE_HOLDS_VALUE(node))
        return fallback;
    type = json_node_get_value_type(node);
    if (type == G_TYPE_DOUBLE)
        return json_node_get_double(node);
    if (type == G_TYPE_INT64)
        return (gdouble) json_node_get_int(node);
    if (type == G_TYPE_INT)
        return (gdouble) json_node_get_int(node);
    return fallback;
}

static const gchar *
json_string(JsonObject *object,
            const gchar *name)
{
    JsonNode *node;

    if (!object || !json_object_has_member(object, name))
        return NULL;
    node = json_object_get_member(object, name);
    return JSON_NODE_HOLDS_VALUE(node) &&
           json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : NULL;
}

static const gchar *
wind_arrow(gdouble degrees)
{
    static const gchar *arrows[] = {
        "↓", "↙", "←", "↖", "↑", "↗", "→", "↘"
    };
    gint index;

    if (!isfinite(degrees))
        return "";
    index = ((gint) lround(fmod(degrees + 180.0 + 360.0, 360.0) / 45.0)) % 8;
    return arrows[index];
}

static void
append_hour(WeatherApplet *applet,
            const WeatherHour *hour)
{
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    GtkWidget *time_label;
    GtkWidget *icon;
    gchar *time = format_short_time(hour->time);
    gchar *temperature = format_temperature(applet, hour->temperature);

    gtk_widget_set_hexpand(column, TRUE);
    gtk_widget_set_halign(column, GTK_ALIGN_CENTER);
    time_label = make_label(time, "weather-small");
    icon = make_icon(weather_icon_name(hour->code, TRUE), 26);
    gtk_box_pack_start(GTK_BOX(column), time_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(column), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(column), make_label(temperature, "weather-value"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(applet->hours_box), column, TRUE, TRUE, 3);

    g_free(time);
    g_free(temperature);
}

static gboolean
draw_day_chart(GtkWidget *widget,
               cairo_t *cr,
               gpointer user_data)
{
    const WeatherDay *day = user_data;
    GtkAllocation allocation;
    gdouble minimum = G_MAXDOUBLE;
    gdouble maximum = -G_MAXDOUBLE;
    gdouble left = 28;
    gdouble right;
    gdouble top = 14;
    gdouble bottom;
    guint i;

    gtk_widget_get_allocation(widget, &allocation);
    right = allocation.width - 18;
    bottom = allocation.height - 30;
    for (i = 0; i < day->n_chart; i++)
    {
        minimum = MIN(minimum, day->chart[i].temperature);
        maximum = MAX(maximum, day->chart[i].temperature);
    }
    if (day->n_chart < 2 || !isfinite(minimum) || !isfinite(maximum))
    {
        cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
        cairo_move_to(cr, left, allocation.height / 2.0);
        cairo_show_text(cr, "Für diesen Tag liegen keine Stundenwerte vor.");
        return FALSE;
    }
    for (i = 0; i < day->n_chart; i++)
    {
        if (!isfinite(day->chart[i].temperature))
        {
            cairo_set_source_rgba(cr, 1, 1, 1, 0.8);
            cairo_move_to(cr, left, allocation.height / 2.0);
            cairo_show_text(cr, "Für diesen Tag fehlen Stundenwerte.");
            return FALSE;
        }
    }
    if (fabs(maximum - minimum) < 0.1)
    {
        maximum += 1;
        minimum -= 1;
    }

    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.24);
    for (i = 0; i < 3; i++)
    {
        gdouble y = top + (bottom - top) * i / 2.0;
        cairo_move_to(cr, left, y);
        cairo_line_to(cr, right, y);
    }
    cairo_stroke(cr);

    cairo_set_source_rgb(cr, 1.0, 0.81, 0.38);
    cairo_set_line_width(cr, 2.5);
    for (i = 0; i < day->n_chart; i++)
    {
        gdouble x = left + (right - left) * i / (day->n_chart - 1);
        gdouble y = bottom - (day->chart[i].temperature - minimum) /
                    (maximum - minimum) * (bottom - top);
        if (i == 0)
            cairo_move_to(cr, x, y);
        else
            cairo_line_to(cr, x, y);
    }
    cairo_stroke(cr);
    for (i = 0; i < day->n_chart; i++)
    {
        gdouble x = left + (right - left) * i / (day->n_chart - 1);
        gdouble y = bottom - (day->chart[i].temperature - minimum) /
                    (maximum - minimum) * (bottom - top);
        cairo_arc(cr, x, y, 3.2, 0, 2 * G_PI);
        cairo_fill(cr);
    }
    cairo_set_source_rgba(cr, 1, 1, 1, 0.82);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                           CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 10);
    for (i = 0; i < day->n_chart; i++)
    {
        cairo_text_extents_t extents;
        gdouble x = left + (right - left) * i / (day->n_chart - 1);
        gchar *time = format_short_time(day->chart[i].time);
        cairo_text_extents(cr, time, &extents);
        cairo_move_to(cr, x - extents.width / 2.0 - extents.x_bearing,
                      allocation.height - 6);
        cairo_show_text(cr, time);
        g_free(time);
    }
    return FALSE;
}

static void
show_hourly_detail(WeatherApplet *applet)
{
    guint i;

    clear_box(applet->hours_box);
    if (applet->selected_day >= 0 &&
        (guint) applet->selected_day < applet->weather->n_days)
    {
        WeatherDay *day = &applet->weather->days[applet->selected_day];
        gchar *date = format_short_date(day->date);
        gchar *title = g_strdup_printf(
            use_english(applet) ? "Day details · %s" : "Tagesverlauf · %s",
            date);
        GtkWidget *chart = gtk_drawing_area_new();
        gtk_widget_set_size_request(chart, 440, 142);
        gtk_widget_set_hexpand(chart, TRUE);
        gtk_widget_set_tooltip_text(chart,
                                    use_english(applet)
                                    ? "Hourly values from Open-Meteo forecast"
                                    : "Stundenwerte aus der Open-Meteo-Vorhersage");
        g_signal_connect(chart, "draw", G_CALLBACK(draw_day_chart), day);
        gtk_label_set_text(GTK_LABEL(applet->hours_title), title);
        gtk_box_pack_start(GTK_BOX(applet->hours_box), chart, TRUE, TRUE, 0);
        gtk_widget_show(chart);
        g_free(title);
        g_free(date);
    }
    else
    {
        gtk_label_set_text(GTK_LABEL(applet->hours_title),
                           use_english(applet) ? "Next hours" : "Nächste Stunden");
        for (i = 0; i < applet->weather->n_hours; i++)
            append_hour(applet, &applet->weather->hours[i]);
    }
}

static gboolean
select_day(GtkWidget *widget,
           GdkEventButton *event,
           gpointer user_data)
{
    WeatherApplet *applet = user_data;
    gint index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "day-index")) - 1;

    (void) event;
    if (index == applet->selected_day)
        applet->selected_day = -1;
    else
        applet->selected_day = index;
    show_hourly_detail(applet);
    return TRUE;
}

static void
append_day(WeatherApplet *applet,
           const WeatherDay *day,
           guint day_index)
{
    GtkWidget *event_box = gtk_event_box_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 9);
    GtkWidget *date_label;
    GtkWidget *icon;
    GtkWidget *bar;
    gchar *date = format_short_date(day->date);
    gchar *minimum = format_temperature(applet, day->minimum);
    gchar *maximum = format_temperature(applet, day->maximum);
    gchar *precipitation;
    if (isfinite(day->snowfall_cm) && day->snowfall_cm >= 1)
        precipitation = day->precipitation_probability >= 0
            ? g_strdup_printf("%d%% · %.0f cm", day->precipitation_probability,
                              day->snowfall_cm)
            : g_strdup_printf("%.0f cm Schnee", day->snowfall_cm);
    else if (isfinite(day->precipitation_mm) && day->precipitation_mm >= 0.1)
        precipitation = day->precipitation_probability >= 0
            ? g_strdup_printf("%d%% · %.0f mm", day->precipitation_probability,
                              day->precipitation_mm)
            : g_strdup_printf("%.0f mm", day->precipitation_mm);
    else
        precipitation = day->precipitation_probability >= 0
            ? g_strdup_printf("%d%%", day->precipitation_probability)
            : g_strdup("—");

    gtk_widget_set_hexpand(row, TRUE);
    date_label = make_label(date, "weather-day");
    gtk_widget_set_size_request(date_label, 45, -1);
    icon = make_icon(weather_icon_name(day->code, TRUE), 25);
    bar = gtk_level_bar_new_for_interval(0, 65);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(bar),
                            (day->minimum + day->maximum) / 2.0 + 20.0);
    gtk_widget_set_hexpand(bar, TRUE);
    gtk_widget_set_size_request(bar, 84, 9);
    gtk_widget_set_tooltip_text(bar, precipitation);
    gtk_box_pack_start(GTK_BOX(row), date_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), make_label(minimum, "weather-small"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), bar, TRUE, TRUE, 3);
    gtk_box_pack_start(GTK_BOX(row), make_label(maximum, "weather-value"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), make_label(precipitation, "weather-small"), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(event_box), row);
    g_object_set_data(G_OBJECT(event_box), "day-index",
                      GINT_TO_POINTER((gint) day_index + 1));
    g_signal_connect(event_box, "button-press-event",
                     G_CALLBACK(select_day), applet);
    gtk_widget_set_tooltip_text(event_box,
                                "Anklicken: Temperaturverlauf; erneut anklicken: Stunden");
    gtk_box_pack_start(GTK_BOX(applet->days_box), event_box, FALSE, FALSE, 2);
    gtk_widget_show_all(event_box);

    g_free(date);
    g_free(minimum);
    g_free(maximum);
    g_free(precipitation);
}

static void
apply_display_options(WeatherApplet *applet)
{
    gtk_widget_set_visible(applet->panel_image, applet->show_panel_icon);
    gtk_widget_set_visible(applet->panel_temperature,
                           applet->show_panel_temperature);
    gtk_widget_set_visible(applet->details, applet->show_details);
    gtk_widget_set_visible(applet->info, applet->show_details);
    gtk_widget_set_visible(applet->extras,
                           applet->show_details &&
                           (applet->show_air_quality || applet->show_moon ||
                            applet->show_measured));
    gtk_widget_set_visible(applet->hours_title, applet->show_forecast);
    gtk_widget_set_visible(applet->hours_box, applet->show_forecast);
    gtk_widget_set_visible(applet->days_title, applet->show_forecast);
    gtk_widget_set_visible(applet->days_box, applet->show_forecast);
}

static gboolean
animate_weather_icon(gpointer user_data)
{
    WeatherApplet *applet = user_data;
    gdouble seconds = g_get_monotonic_time() / 1000000.0;
    gdouble opacity;

    if (!applet->animations || !gtk_widget_get_mapped(applet->weather_window))
    {
        gtk_widget_set_opacity(applet->current_image, 1.0);
        applet->animation_source = 0;
        return G_SOURCE_REMOVE;
    }
    opacity = 0.92 + 0.08 * (0.5 + 0.5 * sin(seconds * G_PI / 3.0));
    gtk_widget_set_opacity(applet->current_image, opacity);
    return G_SOURCE_CONTINUE;
}

static void
set_animation_state(WeatherApplet *applet)
{
    if (applet->animations && gtk_widget_get_mapped(applet->weather_window))
    {
        if (!applet->animation_source)
            applet->animation_source = g_timeout_add(100,
                animate_weather_icon, applet);
    }
    else
    {
        if (applet->animation_source)
        {
            g_source_remove(applet->animation_source);
            applet->animation_source = 0;
        }
        gtk_widget_set_opacity(applet->current_image, 1.0);
    }
}

static void
popup_mapped(GtkWidget *widget,
             gpointer user_data)
{
    (void) widget;
    set_animation_state(user_data);
}

static void
popup_unmapped(GtkWidget *widget,
               gpointer user_data)
{
    (void) widget;
    set_animation_state(user_data);
}

static void
toggle_weather_window(GtkWidget *widget,
                      gpointer user_data)
{
    WeatherApplet *applet = user_data;
    (void) widget;

    if (gtk_widget_get_visible(applet->weather_window))
        gtk_widget_hide(applet->weather_window);
    else
        gtk_window_present(GTK_WINDOW(applet->weather_window));
}

static gboolean
hide_weather_window(GtkWidget *widget,
                    GdkEvent *event,
                    gpointer user_data)
{
    (void) event;
    (void) user_data;
    gtk_widget_hide(widget);
    return TRUE;
}

static void
update_display(WeatherApplet *applet)
{
    Place *place;
    gchar *temperature;
    gchar *minimum;
    gchar *maximum;
    gchar *wind;
    gchar *humidity;
    gchar *feels;
    gchar *uv;
    gchar *sunrise;
    gchar *sunset;
    gchar *details;
    gchar *info;
    gchar *extras;
    gchar *warning_text = NULL;
    gboolean english;
    gboolean pollen_added = FALSE;
    guint i;

    if (applet->places->len == 0 || !applet->weather)
        return;
    place = current_place(applet);
    if (!place)
        return;
    english = use_english(applet);

    gtk_label_set_text(GTK_LABEL(applet->place_name), place->name);
    gtk_image_set_from_icon_name(GTK_IMAGE(applet->current_image),
                                 weather_icon_name(applet->weather->weather_code,
                                                   applet->weather->is_day),
                                 GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(applet->current_image), 78);

    temperature = format_temperature(applet, applet->weather->temperature);
    minimum = format_temperature(applet, applet->weather->today_minimum);
    maximum = format_temperature(applet, applet->weather->today_maximum);
    feels = isfinite(applet->weather->apparent_temperature)
        ? format_temperature(applet, applet->weather->apparent_temperature)
        : g_strdup("—");
    wind = isfinite(applet->weather->wind_speed)
        ? g_strdup_printf("%s %.0f km/h %s",
                          english ? "Wind" : "Wind",
                          applet->weather->wind_speed,
                          wind_arrow(applet->weather->wind_direction))
        : g_strdup("Wind —");
    humidity = isfinite(applet->weather->humidity)
        ? g_strdup_printf("%s %.0f%%", english ? "Humidity" : "Feuchte",
                          applet->weather->humidity)
        : g_strdup(english ? "Humidity —" : "Feuchte —");
    gtk_label_set_text(GTK_LABEL(applet->temperature), temperature);
    gtk_label_set_text(GTK_LABEL(applet->days_title),
                       english ? "7-day forecast" : "7-Tage-Vorschau");
    gtk_label_set_text(GTK_LABEL(applet->summary), english
        ? weather_description_english(applet->weather->weather_code,
                                      applet->weather->is_day)
        : weather_description(applet->weather->weather_code,
                              applet->weather->is_day));
    details = english
        ? g_strdup_printf("%s   ·   feels like %s   ·   %s / %s   ·   %s",
                          wind, feels, maximum, minimum, humidity)
        : g_strdup_printf("%s   ·   gefühlt %s   ·   %s / %s   ·   %s",
                          wind, feels, maximum, minimum, humidity);
    gtk_label_set_text(GTK_LABEL(applet->details), details);
    extras = g_strdup("");
    if (applet->show_air_quality && isfinite(applet->weather->european_aqi))
    {
        gchar *next = g_strdup_printf(english
                                      ? "Air quality (EU index): %.0f"
                                      : "Luftqualität (EU-Index): %.0f",
                                      applet->weather->european_aqi);
        g_free(extras);
        extras = next;
    }
    if (applet->show_air_quality)
    {
        const struct
        {
            const gchar *name_de;
            const gchar *name_en;
            gdouble value;
        } pollen[] = {
            { "Birke", "Birch", applet->weather->birch_pollen },
            { "Gräser", "Grass", applet->weather->grass_pollen },
            { "Erle", "Alder", applet->weather->alder_pollen },
            { "Beifuss", "Mugwort", applet->weather->mugwort_pollen },
            { "Ambrosia", "Ragweed", applet->weather->ragweed_pollen },
            { "Olive", "Olive", applet->weather->olive_pollen }
        };
        guint pollen_index;
        for (pollen_index = 0; pollen_index < G_N_ELEMENTS(pollen);
             pollen_index++)
        {
            if (isfinite(pollen[pollen_index].value))
            {
                const gchar *separator;
                if (pollen_added)
                    separator = " · ";
                else if (*extras)
                    separator = english ? "\nPollen (pollen/m³): "
                                        : "\nPollen (Pollen/m³): ";
                else
                    separator = english ? "Pollen (pollen/m³): "
                                        : "Pollen (Pollen/m³): ";
                gchar *next = g_strdup_printf("%s%s%s %.0f",
                    extras, separator,
                    english ? pollen[pollen_index].name_en
                            : pollen[pollen_index].name_de,
                    pollen[pollen_index].value);
                g_free(extras);
                extras = next;
                pollen_added = TRUE;
            }
        }
    }
    if (applet->show_measured &&
        isfinite(applet->weather->measured_temperature))
    {
        gchar *measured = format_temperature(
            applet, applet->weather->measured_temperature);
        gchar *distance = isfinite(applet->weather->measured_distance_km)
            ? g_strdup_printf(" (%.1f km)",
                              applet->weather->measured_distance_km)
            : g_strdup("");
        gchar *next = g_strdup_printf("%s%s%s: %s — %s%s",
            extras, *extras ? "\n" : "",
            english ? "Station" : "Messstation",
            applet->weather->station_name ? applet->weather->station_name : "DWD",
            measured, distance);
        g_free(measured);
        g_free(distance);
        g_free(extras);
        extras = next;
    }
    if (applet->show_moon && isfinite(applet->weather->moon_phase))
    {
        gdouble phase = fmod(applet->weather->moon_phase + 360.0, 360.0);
        gchar *moonrise = format_short_time(applet->weather->moonrise);
        gchar *moonset = format_short_time(applet->weather->moonset);
        const gchar *name = english
            ? phase < 10 || phase >= 350 ? "New moon"
              : phase < 80 ? "Waxing crescent"
              : phase < 100 ? "First quarter"
              : phase < 170 ? "Waxing gibbous"
              : phase < 190 ? "Full moon"
              : phase < 260 ? "Waning gibbous"
              : phase < 280 ? "Last quarter" : "Waning crescent"
            : phase < 10 || phase >= 350 ? "Neumond"
              : phase < 80 ? "Zunehmende Sichel"
              : phase < 100 ? "Erstes Viertel"
              : phase < 170 ? "Zunehmender Mond"
              : phase < 190 ? "Vollmond"
              : phase < 260 ? "Abnehmender Mond"
              : phase < 280 ? "Letztes Viertel" : "Abnehmende Sichel";
        gchar *next = g_strdup_printf(
            "%s%s%s: %s (%d%%) · ↑ %s · ↓ %s",
            extras, *extras ? "\n" : "", english ? "Moon" : "Mond", name,
            (gint) lround((1.0 - cos(phase * G_PI / 180.0)) * 50.0),
            moonrise, moonset);
        g_free(extras);
        extras = next;
        g_free(moonrise);
        g_free(moonset);
    }
    gtk_label_set_text(GTK_LABEL(applet->extras), extras);

    if (applet->official_warning)
        warning_text = g_strdup(applet->official_warning);
    else if (applet->weather->wind_gusts >= 50)
        warning_text = g_strdup_printf(english
            ? "⚠ Gusts up to %.0f km/h" : "⚠ Böen bis %.0f km/h",
            applet->weather->wind_gusts);
    gtk_label_set_text(GTK_LABEL(applet->warning),
                       warning_text ? warning_text : "");
    gtk_widget_set_visible(applet->warning, warning_text != NULL);

    if (applet->selected_day >= (gint) applet->weather->n_days)
        applet->selected_day = -1;
    show_hourly_detail(applet);
    clear_box(applet->days_box);
    for (i = 0; i < applet->weather->n_days; i++)
        append_day(applet, &applet->weather->days[i], i);

    uv = isfinite(applet->weather->uv_index)
        ? g_strdup_printf("UV %.0f", applet->weather->uv_index)
        : g_strdup("UV —");
    sunrise = format_short_time(applet->weather->sunrise);
    sunset = format_short_time(applet->weather->sunset);
    if (isfinite(applet->weather->precipitation_probability) &&
        isfinite(applet->weather->precipitation_next_2h_mm))
        info = g_strdup_printf(english
            ? "↑ %s   ↓ %s   ·   %s   ·   Rain today %.0f%% · next 2 h %.1f mm"
            : "↑ %s   ↓ %s   ·   %s   ·   Regen heute %.0f%% · nächste 2 h %.1f mm",
            sunrise, sunset, uv, applet->weather->precipitation_probability,
            applet->weather->precipitation_next_2h_mm);
    else if (isfinite(applet->weather->precipitation_probability))
        info = g_strdup_printf(english
            ? "↑ %s   ↓ %s   ·   %s   ·   Rain today %.0f%%"
            : "↑ %s   ↓ %s   ·   %s   ·   Regen heute %.0f%%",
            sunrise, sunset, uv,
            applet->weather->precipitation_probability);
    else
        info = g_strdup_printf("↑ %s   ↓ %s   ·   %s", sunrise, sunset, uv);
    gtk_label_set_text(GTK_LABEL(applet->info), info);
    if (g_strcmp0(applet->provider, "metno") == 0)
        gtk_label_set_markup(GTK_LABEL(applet->source), english
            ? "<a href=\"https://www.met.no/en\">"
              "Current: MET Norway · Forecast: Open-Meteo</a>"
            : "<a href=\"https://www.met.no/en\">"
              "Aktuell: MET Norway · Vorhersage: Open-Meteo</a>");
    else if (g_strcmp0(applet->provider, "dwd") == 0)
        gtk_label_set_markup(GTK_LABEL(applet->source), english
            ? "<a href=\"https://brightsky.dev/\">"
              "Current: DWD / Bright Sky · Forecast: Open-Meteo</a>"
            : "<a href=\"https://brightsky.dev/\">"
              "Aktuell: DWD / Bright Sky · Vorhersage: Open-Meteo</a>");
    else
        gtk_label_set_markup(GTK_LABEL(applet->source), english
            ? "<a href=\"https://open-meteo.com/\">Weather data: Open-Meteo.com</a>"
            : "<a href=\"https://open-meteo.com/\">Wetterdaten: Open-Meteo.com</a>");
    gtk_widget_set_tooltip_text(applet->source,
        "Open-Meteo · MET Norway · DWD/Bright Sky · "
        "Luft/Pollen: Open-Meteo/Copernicus · Warnungen: MeteoAlarm, "
        "GeoSphere, DWD · Ortsuche: GeoNames");

    gtk_image_set_from_icon_name(GTK_IMAGE(applet->panel_image),
                                 weather_icon_name(applet->weather->weather_code,
                                                   applet->weather->is_day),
                                 GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(applet->panel_image),
                             xfce_panel_plugin_get_icon_size(applet->plugin) + 4);
    gtk_label_set_text(GTK_LABEL(applet->panel_temperature), temperature);
    {
        gchar *tooltip = g_strdup_printf("%s: %s · %s",
                                         place->name, temperature,
                                         weather_description(applet->weather->weather_code,
                                                             applet->weather->is_day));
        gtk_widget_set_tooltip_text(applet->button, tooltip);
        g_free(tooltip);
    }
    apply_display_options(applet);

    g_free(temperature);
    g_free(minimum);
    g_free(maximum);
    g_free(feels);
    g_free(wind);
    g_free(humidity);
    g_free(details);
    g_free(warning_text);
    g_free(uv);
    g_free(sunrise);
    g_free(sunset);
    g_free(info);
    g_free(extras);
}

static void
show_fetch_error(WeatherApplet *applet,
                 const gchar *message)
{
    const gchar *text = message && *message
        ? message : "Wetterabruf fehlgeschlagen";

    gtk_label_set_text(GTK_LABEL(applet->status), text);
    gtk_widget_show(applet->status);
    gtk_label_set_text(GTK_LABEL(applet->panel_temperature), "!");
    gtk_widget_set_tooltip_text(applet->button, text);
    g_warning("%s: %s", APP_NAME, text);
}

static void
provider_request_free(ProviderRequest *request)
{
    g_object_unref(request->message);
    g_object_unref(request->plugin);
    g_free(request);
}

static void
provider_loaded(GObject *source,
                GAsyncResult *result,
                gpointer user_data)
{
    ProviderRequest *request = user_data;
    WeatherApplet *applet = get_applet(request->plugin);
    GError *error = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
                                                       result, &error);
    gsize length = 0;
    const gchar *body;
    gboolean applied = FALSE;

    if (!applet || request->serial != applet->request_serial ||
        !applet->weather)
    {
        g_clear_error(&error);
        g_clear_pointer(&bytes, g_bytes_unref);
        provider_request_free(request);
        return;
    }
    if (!bytes)
    {
        gtk_label_set_text(GTK_LABEL(applet->status),
                           error ? error->message : "Wetteranbieter nicht erreichbar");
        gtk_widget_show(applet->status);
        g_warning("%s: selected provider request failed: %s", APP_NAME,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        provider_request_free(request);
        return;
    }
    body = g_bytes_get_data(bytes, &length);
    if (soup_message_get_status(request->message) >= 400)
    {
        gchar *message = g_strdup_printf("Wetteranbieter antwortet mit HTTP %u",
                         soup_message_get_status(request->message));
        gtk_label_set_text(GTK_LABEL(applet->status), message);
        gtk_widget_show(applet->status);
        g_warning("%s: %s", APP_NAME, message);
        g_free(message);
    }
    else if (request->metno)
        applied = weather_data_apply_metno_current(body, length,
                                                   applet->weather, &error);
    else
        applied = weather_data_apply_brightsky_current(body, length,
                                                       applet->weather, &error);
    if (applied)
    {
        gtk_widget_hide(applet->status);
        update_display(applet);
    }
    else if (!error && soup_message_get_status(request->message) < 400)
    {
        error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                   "Selected provider returned unusable weather data");
        gtk_label_set_text(GTK_LABEL(applet->status), error->message);
        gtk_widget_show(applet->status);
        g_warning("%s: %s", APP_NAME, error->message);
    }
    g_clear_error(&error);
    g_bytes_unref(bytes);
    provider_request_free(request);
}

static void
fetch_provider_current(WeatherApplet *applet)
{
    Place *place;
    gchar *latitude;
    gchar *longitude;
    gchar *url;
    SoupMessage *message;
    ProviderRequest *request;
    gboolean metno = g_strcmp0(applet->provider, "metno") == 0;

    if (g_strcmp0(applet->provider, "openmeteo") == 0 ||
        applet->places->len == 0)
        return;
    place = current_place(applet);
    if (!place)
        return;
    latitude = weather_coordinate_to_query(place->latitude);
    longitude = weather_coordinate_to_query(place->longitude);
    if (metno)
        url = g_strdup_printf("https://api.met.no/weatherapi/locationforecast/2.0/compact?"
                              "lat=%s&lon=%s", latitude, longitude);
    else
        url = g_strdup_printf("https://api.brightsky.dev/current_weather?"
                              "lat=%s&lon=%s&max_dist=50000",
                              latitude, longitude);
    g_free(latitude);
    g_free(longitude);
    message = soup_message_new("GET", url);
    g_free(url);
    if (!message)
    {
        gtk_label_set_text(GTK_LABEL(applet->status),
                           "Wetteranbieter-Abfrage konnte nicht vorbereitet werden");
        gtk_widget_show(applet->status);
        return;
    }
    soup_message_headers_append(soup_message_get_request_headers(message),
                                "User-Agent", metno
                                ? APP_USER_AGENT " (MET Norway)"
                                : APP_USER_AGENT);
    request = g_new0(ProviderRequest, 1);
    request->plugin = g_object_ref(applet->plugin);
    request->message = g_object_ref(message);
    request->serial = applet->request_serial;
    request->metno = metno;
    soup_session_send_and_read_async(applet->session, message,
                                     G_PRIORITY_DEFAULT, NULL,
                                     provider_loaded, request);
    g_object_unref(message);
}

static void
auxiliary_request_free(AuxiliaryRequest *request)
{
    g_object_unref(request->message);
    g_object_unref(request->plugin);
    g_free(request);
}

static void
auxiliary_loaded(GObject *source,
                 GAsyncResult *result,
                 gpointer user_data)
{
    AuxiliaryRequest *request = user_data;
    WeatherApplet *applet = get_applet(request->plugin);
    GError *error = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
                                                       result, &error);
    gsize length = 0;
    const gchar *body;
    JsonParser *parser;
    JsonObject *root;

    if (!applet || request->serial != applet->request_serial ||
        !applet->weather)
    {
        g_clear_error(&error);
        g_clear_pointer(&bytes, g_bytes_unref);
        auxiliary_request_free(request);
        return;
    }
    if (!bytes)
    {
        g_warning("%s: auxiliary data request failed: %s", APP_NAME,
                  error ? error->message : "unknown error");
        g_clear_error(&error);
        auxiliary_request_free(request);
        return;
    }
    if (soup_message_get_status(request->message) >= 400)
    {
        g_warning("%s: auxiliary data provider returned HTTP %u", APP_NAME,
                  soup_message_get_status(request->message));
        g_bytes_unref(bytes);
        auxiliary_request_free(request);
        return;
    }
    body = g_bytes_get_data(bytes, &length);
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body, (gssize) length, &error) ||
        !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    {
        g_warning("%s: invalid auxiliary data response: %s", APP_NAME,
                  error ? error->message : "not a JSON object");
        g_clear_error(&error);
        g_object_unref(parser);
        g_bytes_unref(bytes);
        auxiliary_request_free(request);
        return;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    if (request->moon)
    {
        JsonObject *properties =
            json_object_get_object_member(root, "properties");
        JsonObject *moonrise = properties
            ? json_object_get_object_member(properties, "moonrise") : NULL;
        JsonObject *moonset = properties
            ? json_object_get_object_member(properties, "moonset") : NULL;
        applet->weather->moon_phase =
            json_number(properties, "moonphase", NAN);
        g_free(applet->weather->moonrise);
        g_free(applet->weather->moonset);
        applet->weather->moonrise = g_strdup(json_string(moonrise, "time"));
        applet->weather->moonset = g_strdup(json_string(moonset, "time"));
    }
    else
    {
        JsonObject *current = json_object_get_object_member(root, "current");
        applet->weather->european_aqi =
            json_number(current, "european_aqi", NAN);
        applet->weather->birch_pollen =
            json_number(current, "birch_pollen", NAN);
        applet->weather->grass_pollen =
            json_number(current, "grass_pollen", NAN);
        applet->weather->alder_pollen =
            json_number(current, "alder_pollen", NAN);
        applet->weather->mugwort_pollen =
            json_number(current, "mugwort_pollen", NAN);
        applet->weather->ragweed_pollen =
            json_number(current, "ragweed_pollen", NAN);
        applet->weather->olive_pollen =
            json_number(current, "olive_pollen", NAN);
    }
    update_display(applet);
    g_object_unref(parser);
    g_bytes_unref(bytes);
    auxiliary_request_free(request);
}

static void
start_auxiliary_request(WeatherApplet *applet,
                        const gchar *url,
                        gboolean moon)
{
    SoupMessage *message = soup_message_new("GET", url);
    AuxiliaryRequest *request;

    if (!message)
        return;
    soup_message_headers_append(soup_message_get_request_headers(message),
                                "User-Agent", moon
                                ? APP_USER_AGENT " (MET Norway)"
                                : APP_USER_AGENT);
    request = g_new0(AuxiliaryRequest, 1);
    request->plugin = g_object_ref(applet->plugin);
    request->message = g_object_ref(message);
    request->serial = applet->request_serial;
    request->moon = moon;
    soup_session_send_and_read_async(applet->session, message,
                                     G_PRIORITY_DEFAULT, NULL,
                                     auxiliary_loaded, request);
    g_object_unref(message);
}

static void
fetch_auxiliary(WeatherApplet *applet)
{
    Place *place;
    gchar *latitude;
    gchar *longitude;
    gchar *url;

    if (!applet->weather || applet->places->len == 0)
        return;
    place = current_place(applet);
    if (!place)
        return;
    latitude = weather_coordinate_to_query(place->latitude);
    longitude = weather_coordinate_to_query(place->longitude);
    if (applet->show_air_quality)
    {
        url = g_strdup_printf(
            "https://air-quality-api.open-meteo.com/v1/air-quality?"
            "latitude=%s&longitude=%s&current=european_aqi,birch_pollen,"
            "grass_pollen,alder_pollen,mugwort_pollen,ragweed_pollen,"
            "olive_pollen&timezone=auto", latitude, longitude);
        start_auxiliary_request(applet, url, FALSE);
        g_free(url);
    }
    if (applet->show_moon)
    {
        gchar date[11] = "1970-01-01";
        gchar offset[10];
        gint seconds = applet->weather->utc_offset_seconds;
        gint absolute = ABS(seconds);
        const gchar *observed = applet->weather->observed_at;
        if (observed && strlen(observed) >= 10)
            memcpy(date, observed, 10);
        g_snprintf(offset, sizeof(offset), "%s%02d:%02d",
                   seconds < 0 ? "-" : "%2B",
                   absolute / 3600, absolute % 3600 / 60);
        url = g_strdup_printf(
            "https://api.met.no/weatherapi/sunrise/3.0/moon?"
            "lat=%s&lon=%s&date=%s&offset=%s",
            latitude, longitude, date, offset);
        start_auxiliary_request(applet, url, TRUE);
        g_free(url);
    }
    g_free(latitude);
    g_free(longitude);
}

static gboolean
point_in_warning_polygon(gdouble latitude,
                         gdouble longitude,
                         const gchar *text)
{
    gchar **vertices = g_strsplit_set(text ? text : "", " \t\r\n", -1);
    GArray *points = g_array_new(FALSE, FALSE, sizeof(gdouble) * 2);
    gboolean inside = FALSE;
    guint i;

    for (i = 0; vertices[i]; i++)
    {
        gchar **coordinates;
        gchar *end_lat;
        gchar *end_lon;
        gdouble point[2];
        if (!*vertices[i])
            continue;
        coordinates = g_strsplit(vertices[i], ",", 2);
        if (coordinates[0] && coordinates[1])
        {
            point[0] = g_ascii_strtod(coordinates[0], &end_lat);
            point[1] = g_ascii_strtod(coordinates[1], &end_lon);
            if (end_lat != coordinates[0] && *end_lat == '\0' &&
                end_lon != coordinates[1] && *end_lon == '\0')
                g_array_append_vals(points, point, 1);
        }
        g_strfreev(coordinates);
    }
    if (points->len >= 3)
    {
        gdouble (*point)[2] = (gdouble (*)[2]) points->data;
        guint j = points->len - 1;
        for (i = 0; i < points->len; j = i++)
        {
            if ((point[i][1] > longitude) != (point[j][1] > longitude) &&
                latitude < (point[j][0] - point[i][0]) *
                    (longitude - point[i][1]) /
                    (point[j][1] - point[i][1]) + point[i][0])
                inside = !inside;
        }
    }
    g_array_unref(points);
    g_strfreev(vertices);
    return inside;
}

static gboolean
warning_active(JsonObject *object,
               const gchar *start_key,
               const gchar *end_key)
{
    const gchar *end = json_string(object, end_key);
    GDateTime *expires = end ? g_date_time_new_from_iso8601(end, NULL) : NULL;
    gboolean active = !expires ||
        g_date_time_to_unix(expires) >= g_get_real_time() / G_USEC_PER_SEC;

    g_clear_pointer(&expires, g_date_time_unref);
    if (active && start_key)
    {
        const gchar *start = json_string(object, start_key);
        GDateTime *begins = start ? g_date_time_new_from_iso8601(start, NULL) : NULL;
        if (begins)
        {
            active = g_date_time_to_unix(begins) <=
                g_get_real_time() / G_USEC_PER_SEC + 24 * 60 * 60;
            g_date_time_unref(begins);
        }
    }
    return active;
}

static gint
warning_parameter_integer(JsonObject *object,
                          const gchar *name,
                          gint fallback)
{
    JsonArray *parameters = object
        ? json_object_get_array_member(object, "parameter") : NULL;
    guint i;

    for (i = 0; parameters && i < json_array_get_length(parameters); i++)
    {
        JsonObject *parameter = json_array_get_object_element(parameters, i);
        const gchar *value_name = json_string(parameter, "valueName");
        const gchar *value = json_string(parameter, "value");
        gchar *end;
        gint64 number;
        if (g_strcmp0(value_name, name) != 0 || !value)
            continue;
        number = g_ascii_strtoll(value, &end, 10);
        if (end != value && *end == '\0')
            return (gint) number;
    }
    return fallback;
}

static gchar *
warning_from_response(const gchar *body,
                     gsize length,
                     gint country,
                     gdouble latitude,
                     gdouble longitude,
                     guint *severity)
{
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    JsonNode *root_node;
    JsonObject *root;
    gchar *result = NULL;

    *severity = 0;
    if (!json_parser_load_from_data(parser, body, (gssize) length, &error))
    {
        g_clear_error(&error);
        g_object_unref(parser);
        return NULL;
    }
    root_node = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_node))
    {
        g_object_unref(parser);
        return NULL;
    }
    root = json_node_get_object(root_node);
    if (country == 3)
    {
        JsonArray *features = json_object_get_array_member(root, "features");
        guint i;
        for (i = 0; features && i < json_array_get_length(features); i++)
        {
            JsonObject *feature = json_array_get_object_element(features, i);
            JsonObject *properties = feature
                ? json_object_get_object_member(feature, "properties") : NULL;
            const gchar *title = json_string(properties, "HEADLINE");
            const gchar *event = json_string(properties, "EVENT");
            const gchar *level = json_string(properties, "SEVERITY");
            const gchar *severity_text = level ? level : "Minor";
            if (!properties ||
                !warning_active(properties, "ONSET", "EXPIRES"))
                continue;
            *severity = g_strcmp0(severity_text, "Extreme") == 0 ? 4
                : g_strcmp0(severity_text, "Severe") == 0 ? 3
                : g_strcmp0(severity_text, "Moderate") == 0 ? 2 : 1;
            result = g_strdup(title && *title ? title
                               : event && *event ? event : "DWD-Wetterwarnung");
            break;
        }
    }
    else if (country == 2)
    {
        JsonObject *properties = json_object_get_object_member(root, "properties");
        JsonArray *warnings = properties
            ? json_object_get_array_member(properties, "warnings") : NULL;
        guint i;
        for (i = 0; warnings && i < json_array_get_length(warnings); i++)
        {
            JsonObject *warning = json_array_get_object_element(warnings, i);
            JsonObject *details = warning
                ? json_object_get_object_member(warning, "properties") : NULL;
            if (!details)
                details = warning;
            const gchar *text = json_string(details, "text");
            gint64 level = (gint64) json_number(details, "warnstufeid", 1);
            if (!details || !text || !*text)
                continue;
            *severity = (guint) CLAMP(level, 1, 4);
            result = g_strdup(text);
            break;
        }
    }
    else
    {
        JsonArray *warnings = json_object_get_array_member(root, "warnings");
        guint i;
        for (i = 0; warnings && i < json_array_get_length(warnings); i++)
        {
            JsonObject *entry = json_array_get_object_element(warnings, i);
            JsonObject *alert = entry
                ? json_object_get_object_member(entry, "alert") : NULL;
            const gchar *status = json_string(alert, "status");
            const gchar *message_type = json_string(alert, "msgType");
            JsonArray *infos = alert
                ? json_object_get_array_member(alert, "info") : NULL;
            JsonObject *info = infos && json_array_get_length(infos) > 0
                ? json_array_get_object_element(infos, 0) : NULL;
            JsonArray *areas = info
                ? json_object_get_array_member(info, "area") : NULL;
            gboolean covers_location = FALSE;
            guint j;
            const gchar *title;
            if (!info || g_strcmp0(status, "Actual") != 0 ||
                g_strcmp0(message_type, "Cancel") == 0 ||
                !warning_active(info, "onset", "expires"))
                continue;
            for (j = 0; areas && j < json_array_get_length(areas); j++)
            {
                JsonObject *area = json_array_get_object_element(areas, j);
                JsonArray *polygons = area
                    ? json_object_get_array_member(area, "polygon") : NULL;
                guint k;
                for (k = 0; polygons && k < json_array_get_length(polygons); k++)
                {
                    JsonNode *polygon_node = json_array_get_element(polygons, k);
                    if (JSON_NODE_HOLDS_VALUE(polygon_node) &&
                        point_in_warning_polygon(latitude, longitude,
                            json_node_get_string(polygon_node)))
                    {
                        covers_location = TRUE;
                        break;
                    }
                }
                if (covers_location)
                    break;
            }
            if (!covers_location)
                continue;
            *severity = (guint) CLAMP(
                warning_parameter_integer(info, "awareness_level", 2) - 1,
                1, 4);
            title = json_string(info, "event");
            if (!title || !*title)
                title = json_string(info, "headline");
            result = g_strdup(title && *title
                              ? title : "MeteoSchweiz-Wetterwarnung");
            break;
        }
    }
    g_object_unref(parser);
    return result;
}

static void
alert_request_free(AlertRequest *request)
{
    g_object_unref(request->message);
    g_object_unref(request->plugin);
    g_free(request);
}

static void
alerts_loaded(GObject *source,
              GAsyncResult *result,
              gpointer user_data)
{
    AlertRequest *request = user_data;
    WeatherApplet *applet = get_applet(request->plugin);
    GError *error = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
                                                       result, &error);

    if (applet && request->serial == applet->request_serial &&
        applet->warnings_enabled)
    {
        if (!bytes)
        {
            g_warning("%s: official warning request failed: %s", APP_NAME,
                      error ? error->message : "unknown error");
        }
        else if (soup_message_get_status(request->message) < 400)
        {
            gsize length;
            const gchar *body = g_bytes_get_data(bytes, &length);
            guint severity = 0;
            gchar *title = warning_from_response(body, length, request->country,
                request->latitude, request->longitude, &severity);
            g_free(applet->official_warning);
            applet->official_warning = title
                ? (use_english(applet)
                   ? g_strdup_printf("Official %s alert (level %u): %s",
                       request->country == 1 ? "MeteoAlarm" :
                       request->country == 2 ? "GeoSphere" : "DWD",
                       severity, title)
                   : g_strdup_printf("Amtliche %s-Warnung (Stufe %u): %s",
                       request->country == 1 ? "MeteoAlarm" :
                       request->country == 2 ? "GeoSphere" : "DWD",
                       severity, title))
                : NULL;
            if (title && applet->notifications_enabled &&
                severity >= applet->notification_level &&
                g_strcmp0(applet->last_notification, applet->official_warning) != 0)
            {
                gchar *argv[] = { "notify-send", "TuX-Wetter",
                                  applet->official_warning, NULL };
                GError *spawn_error = NULL;
                if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                                   NULL, NULL, NULL, &spawn_error))
                {
                    g_warning("%s: could not send desktop notification: %s",
                              APP_NAME, spawn_error->message);
                    g_clear_error(&spawn_error);
                }
                g_free(applet->last_notification);
                applet->last_notification = g_strdup(applet->official_warning);
            }
            g_free(title);
            update_display(applet);
        }
        else
            g_warning("%s: warning provider returned HTTP %u", APP_NAME,
                      soup_message_get_status(request->message));
    }
    g_clear_error(&error);
    g_clear_pointer(&bytes, g_bytes_unref);
    alert_request_free(request);
}

static void
fetch_alerts(WeatherApplet *applet)
{
    Place *place = current_place(applet);
    gchar *latitude;
    gchar *longitude;
    gchar *url = NULL;
    gint country;
    SoupMessage *message;
    AlertRequest *request;

    if (!applet->warnings_enabled || !place)
        return;
    latitude = weather_coordinate_to_query(place->latitude);
    longitude = weather_coordinate_to_query(place->longitude);
    if (place->latitude >= 45.8 && place->latitude <= 47.85 &&
        place->longitude >= 5.9 && place->longitude <= 10.5)
    {
        country = 1;
        url = g_strdup("https://feeds.meteoalarm.org/api/v1/warnings/feeds-switzerland");
    }
    else if (place->latitude >= 46.3 && place->latitude <= 49.1 &&
             place->longitude >= 9.5 && place->longitude <= 17.2)
    {
        country = 2;
        url = g_strdup_printf(
            "https://warnungen.zamg.at/wsapp/api/getWarningsForCoords?"
            "lon=%s&lat=%s&lang=de", longitude, latitude);
    }
    else if (place->latitude >= 47.2 && place->latitude <= 55.1 &&
             place->longitude >= 5.8 && place->longitude <= 15.1)
    {
        gchar *point = g_strdup_printf("INTERSECTS(THE_GEOM,POINT(%s %s))",
                                       latitude, longitude);
        gchar *escaped = g_uri_escape_string(point, NULL, TRUE);
        country = 3;
        url = g_strdup_printf(
            "https://maps.dwd.de/geoserver/dwd/ows?service=WFS&version=2.0.0"
            "&request=GetFeature&typeName=dwd:Warnungen_Gemeinden"
            "&outputFormat=application%%2Fjson&propertyName=EVENT,SEVERITY,"
            "ONSET,EXPIRES,HEADLINE&CQL_FILTER=%s", escaped);
        g_free(escaped);
        g_free(point);
    }
    else
        country = 0;
    g_free(latitude);
    g_free(longitude);
    if (!country)
        return;
    message = soup_message_new("GET", url);
    g_free(url);
    if (!message)
        return;
    soup_message_headers_append(soup_message_get_request_headers(message),
                                "User-Agent", APP_USER_AGENT);
    request = g_new0(AlertRequest, 1);
    request->plugin = g_object_ref(applet->plugin);
    request->message = g_object_ref(message);
    request->serial = applet->request_serial;
    request->country = country;
    request->latitude = place->latitude;
    request->longitude = place->longitude;
    soup_session_send_and_read_async(applet->session, message,
                                     G_PRIORITY_DEFAULT, NULL,
                                     alerts_loaded, request);
    g_object_unref(message);
}

static void
auto_location_loaded(GObject *source,
                     GAsyncResult *result,
                     gpointer user_data)
{
    AutoLocationRequest *request = user_data;
    WeatherApplet *applet = get_applet(request->plugin);
    GError *error = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
                                                       result, &error);

    if (applet && applet->auto_location)
    {
        if (!bytes)
        {
            gtk_label_set_text(GTK_LABEL(applet->status),
                error ? error->message : "Automatische Ortung fehlgeschlagen");
            gtk_widget_show(applet->status);
            g_warning("%s: automatic location lookup failed: %s", APP_NAME,
                      error ? error->message : "unknown error");
        }
        else if (soup_message_get_status(request->message) >= 400)
        {
            gchar *message = g_strdup_printf(
                "Standortdienst antwortet mit HTTP %u",
                soup_message_get_status(request->message));
            gtk_label_set_text(GTK_LABEL(applet->status), message);
            gtk_widget_show(applet->status);
            g_warning("%s: %s", APP_NAME, message);
            g_free(message);
        }
        else
        {
            gsize length;
            const gchar *body = g_bytes_get_data(bytes, &length);
            JsonParser *parser = json_parser_new();
            if (json_parser_load_from_data(parser, body, (gssize) length,
                                           &error) &&
                JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
            {
                JsonObject *root = json_node_get_object(
                    json_parser_get_root(parser));
                gdouble lat = json_number(root, "latitude", NAN);
                gdouble lon = json_number(root, "longitude", NAN);
                const gchar *city = json_string(root, "city");
                const gchar *region = json_string(root, "region");
                if (isfinite(lat) && isfinite(lon) &&
                    lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)
                {
                    gchar *name = city && *city
                        ? g_strdup(city)
                        : g_strdup(region && *region ? region : "Automatischer Standort");
                    place_free(applet->auto_place);
                    applet->auto_place = place_new(name, lat, lon);
                    g_free(name);
                    save_settings(applet);
                    gtk_widget_hide(applet->status);
                    fetch_weather(applet);
                }
                else
                {
                    gtk_label_set_text(GTK_LABEL(applet->status),
                        "Standortdienst lieferte ungültige Koordinaten.");
                    gtk_widget_show(applet->status);
                }
            }
            else
            {
                gtk_label_set_text(GTK_LABEL(applet->status),
                    error ? error->message : "Ungültige Standortantwort");
                gtk_widget_show(applet->status);
            }
            g_clear_error(&error);
            g_object_unref(parser);
        }
    }
    g_clear_error(&error);
    g_clear_pointer(&bytes, g_bytes_unref);
    g_object_unref(request->message);
    g_object_unref(request->plugin);
    g_free(request);
}

static void
fetch_auto_location(WeatherApplet *applet)
{
    SoupMessage *message;
    AutoLocationRequest *request;

    if (!applet->auto_location)
        return;
    if (applet->auto_place)
    {
        fetch_weather(applet);
        return;
    }
    message = soup_message_new("GET", "https://get.geojs.io/v1/ip/geo.json");
    if (!message)
    {
        gtk_label_set_text(GTK_LABEL(applet->status),
                           "Standortabfrage konnte nicht vorbereitet werden.");
        gtk_widget_show(applet->status);
        return;
    }
    soup_message_headers_append(soup_message_get_request_headers(message),
                                "User-Agent", APP_USER_AGENT);
    request = g_new0(AutoLocationRequest, 1);
    request->plugin = g_object_ref(applet->plugin);
    request->message = g_object_ref(message);
    soup_session_send_and_read_async(applet->session, message,
                                     G_PRIORITY_DEFAULT, NULL,
                                     auto_location_loaded, request);
    g_object_unref(message);
}

static void
clear_weather_for_place_change(WeatherApplet *applet)
{
    clear_box(applet->hours_box);
    gtk_label_set_text(GTK_LABEL(applet->hours_title), "Nächste Stunden");
    weather_data_free(applet->weather);
    applet->weather = NULL;
    applet->selected_day = -1;
    gtk_label_set_text(GTK_LABEL(applet->panel_temperature), "--");
    gtk_label_set_text(GTK_LABEL(applet->temperature), "--");
    gtk_label_set_text(GTK_LABEL(applet->summary), "Lade Wetterdaten …");
    gtk_label_set_text(GTK_LABEL(applet->details), "");
    gtk_label_set_text(GTK_LABEL(applet->extras), "");
    {
        Place *place = current_place(applet);
        gtk_label_set_text(GTK_LABEL(applet->place_name),
                           place ? place->name : "Kein Ort ausgewählt");
    }
    gtk_label_set_text(GTK_LABEL(applet->warning), "");
    gtk_widget_hide(applet->warning);
    g_clear_pointer(&applet->official_warning, g_free);
    g_clear_pointer(&applet->last_notification, g_free);
}

static void
apply_theme(WeatherApplet *applet)
{
    static const struct
    {
        const gchar *name;
        const gchar *top;
        const gchar *bottom;
    } themes[] = {
        { "violett", "rgba(56,48,106,0.94)", "rgba(38,31,80,0.94)" },
        { "blau", "rgba(24,94,145,0.94)", "rgba(18,58,112,0.94)" },
        { "petrol", "rgba(8,124,131,0.94)", "rgba(7,86,94,0.94)" },
        { "gruen", "rgba(56,128,77,0.94)", "rgba(36,93,59,0.94)" },
        { "bordeaux", "rgba(140,61,87,0.94)", "rgba(97,43,74,0.94)" },
        { "sonnenuntergang", "rgba(160,91,67,0.94)", "rgba(100,52,85,0.94)" },
        { "graphit", "rgba(81,89,102,0.94)", "rgba(48,54,65,0.94)" },
        { "nacht", "rgba(26,39,58,0.94)", "rgba(16,24,39,0.94)" }
    };
    const gchar *top = themes[0].top;
    const gchar *bottom = themes[0].bottom;
    GtkCssProvider *provider;
    GtkStyleContext *context;
    gchar *css;
    guint i;

    for (i = 0; i < G_N_ELEMENTS(themes); i++)
    {
        if (g_strcmp0(applet->theme, themes[i].name) == 0)
        {
            top = themes[i].top;
            bottom = themes[i].bottom;
            break;
        }
    }

    css = g_strdup_printf(
        "window#tux-wetter-popup {"
        " background-image: linear-gradient(145deg, %s 0%%, %s 100%%);"
        " border: 1px solid rgba(255,255,255,0.28);"
        " border-radius: 20px; color: #ffffff; padding: 18px; }"
        "window#tux-wetter-popup label { color: #ffffff; }"
        "window#tux-wetter-popup .weather-title { font-size: 15pt; font-weight: 600; }"
        "window#tux-wetter-popup .weather-temperature { font-size: 34pt; font-weight: 300; }"
        "window#tux-wetter-popup .weather-value { font-weight: 600; }"
        "window#tux-wetter-popup .weather-small { opacity: 0.82; font-size: 9pt; }"
        "window#tux-wetter-popup .weather-day { font-weight: 600; }"
        "window#tux-wetter-popup .weather-separator { background: rgba(255,255,255,0.30); min-height: 1px; }"
        "window#tux-wetter-popup button { color: #ffffff; background: rgba(255,255,255,0.12);"
        " border: 1px solid rgba(255,255,255,0.20); border-radius: 8px; }"
        "window#tux-wetter-popup button:hover { background: rgba(255,255,255,0.24); }"
        "window#tux-wetter-popup levelbar trough { background: rgba(255,255,255,0.22);"
        " border-radius: 4px; min-height: 5px; }"
        "window#tux-wetter-popup levelbar block.filled { background: #ffcf69; border-radius: 4px; }"
        "window#tux-wetter-popup .weather-warning { color: #ffdf83; font-weight: 600; }"
        "window#tux-wetter-popup .weather-source { opacity: 0.65; font-size: 8pt; }",
        top, bottom);
    provider = gtk_css_provider_new();
    if (!gtk_css_provider_load_from_data(provider, css, -1, NULL))
        g_warning("%s: could not load widget style", APP_NAME);
    context = gtk_widget_get_style_context(applet->weather_window);
    gtk_style_context_add_provider(context,
                                   GTK_STYLE_PROVIDER(provider),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
    g_free(css);
}

static void
weather_loaded(GObject *source,
               GAsyncResult *result,
               gpointer user_data)
{
    WeatherRequest *request = user_data;
    WeatherApplet *applet = get_applet(request->plugin);
    GError *error = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
                                                       result, &error);
    gsize length = 0;
    const gchar *body;
    WeatherData *weather;

    if (!applet)
    {
        g_clear_error(&error);
        g_clear_pointer(&bytes, g_bytes_unref);
        g_object_unref(request->message);
        g_object_unref(request->plugin);
        g_free(request);
        return;
    }
    if (request->serial != applet->request_serial)
    {
        g_clear_error(&error);
        g_clear_pointer(&bytes, g_bytes_unref);
        g_object_unref(request->message);
        g_object_unref(request->plugin);
        g_free(request);
        return;
    }
    if (!bytes)
    {
        show_fetch_error(applet, error ? error->message : NULL);
        g_clear_error(&error);
        g_object_unref(request->message);
        g_object_unref(request->plugin);
        g_free(request);
        return;
    }
    body = g_bytes_get_data(bytes, &length);
    if (soup_message_get_status(request->message) >= 400)
    {
        gchar *message = weather_response_error(body, length,
                                                soup_message_get_status(request->message));
        show_fetch_error(applet, message);
        g_free(message);
        g_bytes_unref(bytes);
        g_object_unref(request->message);
        g_object_unref(request->plugin);
        g_free(request);
        return;
    }

    weather = weather_data_parse(body, length, &error);
    g_bytes_unref(bytes);
    if (!weather)
    {
        show_fetch_error(applet, error ? error->message : NULL);
        g_clear_error(&error);
        g_object_unref(request->message);
        g_object_unref(request->plugin);
        g_free(request);
        return;
    }

    clear_box(applet->hours_box);
    weather_data_free(applet->weather);
    applet->weather = weather;
    gtk_widget_hide(applet->status);
    update_display(applet);
    fetch_provider_current(applet);
    fetch_auxiliary(applet);
    fetch_alerts(applet);
    g_object_unref(request->message);
    g_object_unref(request->plugin);
    g_free(request);
}

static void
preferences_changed(GtkWidget *widget,
                   gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(user_data);
    WeatherApplet *applet = g_object_get_data(G_OBJECT(dialog), "applet");
    GtkWidget *unit = g_object_get_data(G_OBJECT(dialog), "unit");
    GtkWidget *theme = g_object_get_data(G_OBJECT(dialog), "theme");
    GtkWidget *refresh = g_object_get_data(G_OBJECT(dialog), "refresh");
    GtkWidget *panel_icon = g_object_get_data(G_OBJECT(dialog), "panel-icon");
    GtkWidget *panel_temperature = g_object_get_data(G_OBJECT(dialog),
                                                     "panel-temperature");
    GtkWidget *details = g_object_get_data(G_OBJECT(dialog), "details");
    GtkWidget *forecast = g_object_get_data(G_OBJECT(dialog), "forecast");
    GtkWidget *saved_places = g_object_get_data(G_OBJECT(dialog),
                                                "saved-places");
    GtkWidget *provider = g_object_get_data(G_OBJECT(dialog), "provider");
    GtkWidget *air_quality = g_object_get_data(G_OBJECT(dialog), "air-quality");
    GtkWidget *moon = g_object_get_data(G_OBJECT(dialog), "moon");
    GtkWidget *measured = g_object_get_data(G_OBJECT(dialog), "measured");
    GtkWidget *auto_location = g_object_get_data(G_OBJECT(dialog), "auto-location");
    GtkWidget *warnings = g_object_get_data(G_OBJECT(dialog), "warnings");
    GtkWidget *notifications = g_object_get_data(G_OBJECT(dialog), "notifications");
    GtkWidget *notify_level = g_object_get_data(G_OBJECT(dialog), "notify-level");
    GtkWidget *language = g_object_get_data(G_OBJECT(dialog), "language");
    GtkWidget *animations = g_object_get_data(G_OBJECT(dialog), "animations");
    gchar *selected_provider;
    gchar *selected_theme;
    gint selected_place;
    gboolean place_changed;
    gboolean provider_changed;
    gboolean auto_location_changed;
    gboolean alerts_changed;
    gboolean language_changed;
    gchar *selected_language;

    (void) widget;
    if (!applet)
        return;

    applet->fahrenheit = gtk_combo_box_get_active(GTK_COMBO_BOX(unit)) == 1;
    applet->refresh_minutes = (guint) gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(refresh));
    applet->show_panel_icon =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(panel_icon));
    applet->show_panel_temperature =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(panel_temperature));
    applet->show_details =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(details));
    applet->show_forecast =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(forecast));
    applet->show_air_quality =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(air_quality));
    applet->show_moon =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(moon));
    applet->show_measured =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(measured));
    applet->animations =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(animations));
    auto_location_changed = applet->auto_location !=
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_location));
    applet->auto_location =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_location));
    alerts_changed = applet->warnings_enabled !=
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(warnings)) ||
        applet->notifications_enabled !=
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(notifications));
    applet->warnings_enabled =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(warnings));
    applet->notifications_enabled =
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(notifications));
    applet->notification_level = (guint) gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(notify_level));
    selected_language = g_strdup(gtk_combo_box_get_active_id(
        GTK_COMBO_BOX(language)));
    language_changed = selected_language &&
        g_strcmp0(selected_language, applet->language) != 0;
    if (selected_language)
    {
        g_free(applet->language);
        applet->language = selected_language;
    }
    selected_provider = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(provider));
    provider_changed = selected_provider &&
        g_strcmp0(selected_provider, applet->provider) != 0;
    if (selected_provider)
    {
        g_free(applet->provider);
        applet->provider = selected_provider;
    }
    selected_place = gtk_combo_box_get_active(GTK_COMBO_BOX(saved_places));
    place_changed = selected_place >= 0 &&
        (guint) selected_place < applet->places->len &&
        (guint) selected_place != applet->place_index;
    if (selected_place >= 0 &&
        (guint) selected_place < applet->places->len)
        applet->place_index = (guint) selected_place;
    if (place_changed && applet->auto_location)
    {
        applet->auto_location = FALSE;
        auto_location_changed = TRUE;
        g_signal_handlers_block_by_func(auto_location,
                                        G_CALLBACK(preferences_changed),
                                        dialog);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_location), FALSE);
        g_signal_handlers_unblock_by_func(auto_location,
                                          G_CALLBACK(preferences_changed),
                                          dialog);
    }

    selected_theme = gtk_combo_box_text_get_active_text(
        GTK_COMBO_BOX_TEXT(theme));
    if (selected_theme)
    {
        g_free(applet->theme);
        applet->theme = selected_theme;
    }

    apply_theme(applet);
    apply_display_options(applet);
    set_animation_state(applet);
    if (auto_location_changed && applet->auto_location)
        fetch_auto_location(applet);
    else if (place_changed || provider_changed || auto_location_changed)
    {
        clear_weather_for_place_change(applet);
        fetch_weather(applet);
    }
    else if (applet->weather)
    {
        update_display(applet);
        fetch_auxiliary(applet);
    }
    if (alerts_changed)
    {
        if (!applet->warnings_enabled)
            g_clear_pointer(&applet->official_warning, g_free);
        update_display(applet);
        fetch_alerts(applet);
    }
    if (language_changed && applet->weather)
        update_display(applet);

    if (applet->refresh_source)
        g_source_remove(applet->refresh_source);
    if (applet->animation_source)
        g_source_remove(applet->animation_source);
    applet->refresh_source = g_timeout_add_seconds(
        applet->refresh_minutes * 60, refresh_timeout, applet);
    save_settings(applet);
}

static void
fetch_weather(WeatherApplet *applet)
{
    Place *place;
    gchar *latitude;
    gchar *longitude;
    gchar *url;
    SoupMessage *message;
    WeatherRequest *request;

    if (applet->places->len == 0)
        return;
    place = current_place(applet);
    if (!place)
        return;
    latitude = weather_coordinate_to_query(place->latitude);
    longitude = weather_coordinate_to_query(place->longitude);
    url = g_strdup_printf(
        "https://api.open-meteo.com/v1/forecast?"
        "latitude=%s&longitude=%s&current=temperature_2m,relative_humidity_2m,"
        "apparent_temperature,is_day,precipitation,weather_code,wind_speed_10m,"
        "wind_direction_10m,wind_gusts_10m&hourly=temperature_2m,weather_code,"
        "wind_gusts_10m,precipitation,precipitation_probability&daily=weather_code,temperature_2m_max,"
        "temperature_2m_min,precipitation_probability_max,precipitation_sum,"
        "snowfall_sum,sunrise,sunset,uv_index_max&timezone=auto&forecast_days=7",
        latitude, longitude);
    g_free(latitude);
    g_free(longitude);
    message = soup_message_new("GET", url);
    g_free(url);
    if (!message)
    {
        gtk_label_set_text(GTK_LABEL(applet->status),
                           "Wetterabfrage konnte nicht vorbereitet werden");
        gtk_widget_show(applet->status);
        return;
    }
    soup_message_headers_append(soup_message_get_request_headers(message),
                                "User-Agent", APP_USER_AGENT);
    gtk_label_set_text(GTK_LABEL(applet->status), "Lade Wetterdaten …");
    gtk_widget_show(applet->status);
    request = g_new0(WeatherRequest, 1);
    request->plugin = g_object_ref(applet->plugin);
    request->message = g_object_ref(message);
    request->serial = ++applet->request_serial;
    soup_session_send_and_read_async(applet->session, message,
                                     G_PRIORITY_DEFAULT, NULL,
                                     weather_loaded,
                                     request);
    g_object_unref(message);
}

static gboolean
refresh_timeout(gpointer user_data)
{
    fetch_weather(user_data);
    return G_SOURCE_CONTINUE;
}

static void
change_place(GtkButton *button,
             gpointer user_data)
{
    WeatherApplet *applet = user_data;
    gint direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),
                                                       "direction"));
    gint count = (gint) applet->places->len;

    if (count < 2)
    {
        if (applet->auto_location)
        {
            applet->auto_location = FALSE;
            save_settings(applet);
            clear_weather_for_place_change(applet);
            fetch_weather(applet);
        }
        return;
    }
    applet->auto_location = FALSE;
    applet->place_index = (guint) ((gint) applet->place_index +
                                  (direction > 0 ? 1 : count - 1)) % (guint) count;
    save_settings(applet);
    clear_weather_for_place_change(applet);
    fetch_weather(applet);
}

static gboolean
panel_size_changed(XfcePanelPlugin *plugin,
                  gint size,
                  gpointer user_data)
{
    WeatherApplet *applet = user_data;
    (void) size;
    gtk_image_set_pixel_size(GTK_IMAGE(applet->panel_image),
                             xfce_panel_plugin_get_icon_size(plugin) + 4);
    return TRUE;
}

static void
add_separator(GtkWidget *box)
{
    GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(separator),
                                "weather-separator");
    gtk_box_pack_start(GTK_BOX(box), separator, FALSE, FALSE, 1);
}

static GtkWidget *
build_weather_window(WeatherApplet *applet)
{
    gboolean english = use_english(applet);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *previous = gtk_button_new_with_label("‹");
    GtkWidget *next = gtk_button_new_with_label("›");
    GtkWidget *settings = gtk_button_new_with_label("⚙");
    GtkWidget *close = gtk_button_new_from_icon_name("window-close",
                                                     GTK_ICON_SIZE_MENU);
    GtkWidget *current = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    GtkWidget *current_text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    GtkWidget *current_top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *hours_scroller;
    GtkWidget *scroller;
    GtkWidget *settings_icon;
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);

    applet->weather_window = window;
    gtk_window_set_title(GTK_WINDOW(window), "TuX-Wetter");
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(window), GDK_WINDOW_TYPE_HINT_UTILITY);
    gtk_widget_set_name(window, "tux-wetter-popup");
    gtk_widget_set_size_request(window, 500, -1);
    gtk_container_add(GTK_CONTAINER(window), outer);
    g_signal_connect(window, "delete-event",
                     G_CALLBACK(hide_weather_window), applet);
    g_signal_connect(close, "clicked",
                     G_CALLBACK(toggle_weather_window), applet);

    g_object_set_data(G_OBJECT(previous), "direction", GINT_TO_POINTER(-1));
    g_object_set_data(G_OBJECT(next), "direction", GINT_TO_POINTER(1));
    g_signal_connect(previous, "clicked", G_CALLBACK(change_place), applet);
    g_signal_connect(next, "clicked", G_CALLBACK(change_place), applet);
    settings_icon = gtk_image_new_from_icon_name("preferences-system",
                                                 GTK_ICON_SIZE_MENU);
    gtk_button_set_image(GTK_BUTTON(settings), settings_icon);
    gtk_button_set_relief(GTK_BUTTON(previous), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(next), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(settings), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
    g_signal_connect_swapped(settings, "clicked",
                             G_CALLBACK(show_preferences), applet);

    applet->place_name = make_label(english ? "Loading location …" : "Lade Ort …",
                                    "weather-title");
    gtk_widget_set_hexpand(applet->place_name, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(applet->place_name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(applet->place_name), 28);
    gtk_box_pack_start(GTK_BOX(header), previous, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), applet->place_name, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(header), next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), settings, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(header), close, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);

    applet->current_image = make_icon("weather-few-clouds", 78);
    gtk_box_pack_start(GTK_BOX(icon_box), applet->current_image, FALSE, FALSE, 0);
    applet->temperature = make_label("--", "weather-temperature");
    applet->summary = make_label(english ? "Loading weather" :
                                 "Wetterdaten werden geladen", "weather-value");
    gtk_label_set_line_wrap(GTK_LABEL(applet->summary), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(applet->summary), 22);
    applet->details = make_label("", "weather-small");
    gtk_label_set_line_wrap(GTK_LABEL(applet->details), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(applet->details), 38);
    gtk_box_pack_start(GTK_BOX(current_top), applet->temperature, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(current_top), applet->summary, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(current_text), current_top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(current_text), applet->details, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(current), icon_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(current), current_text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), current, FALSE, FALSE, 0);

    applet->warning = make_label("", "weather-warning");
    gtk_label_set_line_wrap(GTK_LABEL(applet->warning), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(applet->warning), 38);
    gtk_widget_set_halign(applet->warning, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(outer), applet->warning, FALSE, FALSE, 0);
    applet->status = make_label(english ? "Loading weather data …" :
                                "Lade Wetterdaten …", "weather-small");
    gtk_label_set_line_wrap(GTK_LABEL(applet->status), TRUE);
    gtk_widget_set_halign(applet->status, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(outer), applet->status, FALSE, FALSE, 0);

    add_separator(outer);
    applet->hours_title = make_label(english ? "Next hours" : "Nächste Stunden",
                                     "weather-day");
    gtk_box_pack_start(GTK_BOX(outer), applet->hours_title, FALSE, FALSE, 0);
    applet->hours_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 7);
    hours_scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(hours_scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_widget_set_size_request(hours_scroller, 440, -1);
    gtk_container_add(GTK_CONTAINER(hours_scroller), applet->hours_box);
    gtk_box_pack_start(GTK_BOX(outer), hours_scroller, FALSE, FALSE, 0);

    applet->info = make_label(english ? "Sunrise · UV · Rain" :
                              "Sonnenzeiten · UV · Regen", "weather-small");
    gtk_label_set_line_wrap(GTK_LABEL(applet->info), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(applet->info), 42);
    gtk_widget_set_halign(applet->info, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(outer), applet->info, FALSE, FALSE, 2);
    applet->extras = make_label("", "weather-small");
    gtk_label_set_line_wrap(GTK_LABEL(applet->extras), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(applet->extras), 42);
    gtk_widget_set_halign(applet->extras, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(outer), applet->extras, FALSE, FALSE, 2);
    add_separator(outer);
    applet->days_title = make_label(english ? "7-day forecast" :
                                    "7-Tage-Vorschau", "weather-day");
    gtk_box_pack_start(GTK_BOX(outer), applet->days_title, FALSE, FALSE, 0);
    applet->days_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroller, -1, 238);
    gtk_container_add(GTK_CONTAINER(scroller), applet->days_box);
    gtk_box_pack_start(GTK_BOX(outer), scroller, FALSE, FALSE, 0);

    applet->source = make_label("Wetterdaten: Open-Meteo.com", "weather-source");
    gtk_widget_set_halign(applet->source, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(outer), applet->source, FALSE, FALSE, 0);
    apply_theme(applet);
    gtk_widget_show_all(outer);
    return applet->weather_window;
}

static void
search_context_free(gpointer data)
{
    SearchContext *context = data;
    g_object_unref(context->applet->plugin);
    g_object_unref(context->results);
    g_object_unref(context->status);
    g_object_unref(context->message);
    g_free(context);
}

static void
search_loaded(GObject *source,
              GAsyncResult *result,
              gpointer user_data)
{
    SearchContext *context = user_data;
    GError *error = NULL;
    GBytes *bytes = soup_session_send_and_read_finish(SOUP_SESSION(source),
                                                       result, &error);
    JsonParser *parser;
    JsonObject *root;
    JsonArray *places;
    gsize length;
    const gchar *body;
    guint i;

    if (!bytes)
    {
        gtk_label_set_text(GTK_LABEL(context->status), error->message);
        g_clear_error(&error);
        search_context_free(context);
        return;
    }
    if (soup_message_get_status(context->message) >= 400)
    {
        gchar *message = g_strdup_printf("Ortssuche antwortet mit HTTP %u",
                                         soup_message_get_status(context->message));
        gtk_label_set_text(GTK_LABEL(context->status), message);
        g_free(message);
        g_bytes_unref(bytes);
        search_context_free(context);
        return;
    }
    body = g_bytes_get_data(bytes, &length);
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, body, (gssize) length, &error))
    {
        gtk_label_set_text(GTK_LABEL(context->status), error->message);
        g_clear_error(&error);
        g_object_unref(parser);
        g_bytes_unref(bytes);
        search_context_free(context);
        return;
    }
    if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
    {
        gtk_label_set_text(GTK_LABEL(context->status),
                           "Die Ortssuche lieferte eine ungültige Antwort.");
        g_object_unref(parser);
        g_bytes_unref(bytes);
        search_context_free(context);
        return;
    }
    root = json_node_get_object(json_parser_get_root(parser));
    places = json_object_get_array_member(root, "results");
    if (places)
    {
        for (i = 0; i < json_array_get_length(places); i++)
        {
            JsonObject *item = json_array_get_object_element(places, i);
            const gchar *name = item ? json_string(item, "name") : NULL;
            const gchar *country = item ? json_string(item, "country") : NULL;
            const gchar *admin = item ? json_string(item, "admin1") : NULL;
            gdouble lat;
            gdouble lon;
            gchar *display;
            GtkWidget *row;
            GtkWidget *label;
            Place *place;

            if (!name || !item)
                continue;
            lat = json_number(item, "latitude", NAN);
            lon = json_number(item, "longitude", NAN);
            if (!isfinite(lat) || !isfinite(lon))
                continue;
            display = admin && *admin
                ? g_strdup_printf("%s, %s, %s", name, admin, country ? country : "")
                : g_strdup_printf("%s, %s", name, country ? country : "");
            row = gtk_list_box_row_new();
            label = gtk_label_new(display);
            gtk_widget_set_halign(label, GTK_ALIGN_START);
            gtk_container_add(GTK_CONTAINER(row), label);
            place = place_new(name, lat, lon);
            g_object_set_data_full(G_OBJECT(row), "tux-place", place, place_free);
            gtk_container_add(GTK_CONTAINER(context->results), row);
            g_free(display);
        }
    }
    if (!places || json_array_get_length(places) == 0)
        gtk_label_set_text(GTK_LABEL(context->status), "Keine Orte gefunden.");
    else
        gtk_label_set_text(GTK_LABEL(context->status),
                           "Ort auswählen und „Hinzufügen“ anklicken.");
    gtk_widget_show_all(context->results);
    g_object_unref(parser);
    g_bytes_unref(bytes);
    search_context_free(context);
}

static void
search_places(GtkButton *button,
              gpointer user_data)
{
    WeatherApplet *applet = user_data;
    GtkWidget *entry = g_object_get_data(G_OBJECT(button), "entry");
    GtkWidget *results = g_object_get_data(G_OBJECT(button), "results");
    GtkWidget *status = g_object_get_data(G_OBJECT(button), "status");
    const gchar *query = gtk_entry_get_text(GTK_ENTRY(entry));
    gchar *escaped;
    gchar *url;
    SoupMessage *message;
    SearchContext *context;

    if (!*query)
    {
        gtk_label_set_text(GTK_LABEL(status), "Bitte einen Ort eingeben.");
        return;
    }
    clear_box(results);
    gtk_label_set_text(GTK_LABEL(status), "Suche läuft …");
    escaped = g_uri_escape_string(query, NULL, TRUE);
    url = g_strdup_printf("https://geocoding-api.open-meteo.com/v1/search?"
                          "name=%s&count=10&language=de&format=json", escaped);
    g_free(escaped);
    message = soup_message_new("GET", url);
    g_free(url);
    if (!message)
    {
        gtk_label_set_text(GTK_LABEL(status), "Ortssuche konnte nicht gestartet werden.");
        return;
    }
    soup_message_headers_append(soup_message_get_request_headers(message),
                                "User-Agent", APP_USER_AGENT);
    context = g_new0(SearchContext, 1);
    context->applet = applet;
    context->results = g_object_ref(results);
    context->status = g_object_ref(status);
    context->message = g_object_ref(message);
    g_object_ref(applet->plugin);
    soup_session_send_and_read_async(applet->session, message,
                                     G_PRIORITY_DEFAULT, NULL,
                                     search_loaded, context);
    g_object_unref(message);
}

static void
populate_saved_places(WeatherApplet *applet,
                     GtkComboBoxText *combo)
{
    guint i;

    gtk_combo_box_text_remove_all(combo);
    for (i = 0; i < applet->places->len; i++)
    {
        Place *place = g_ptr_array_index(applet->places, i);
        gtk_combo_box_text_append_text(combo, place->name);
    }
    if (applet->places->len > 0)
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo),
                                 (gint) applet->place_index);
}

static void
add_selected_place(GtkButton *button,
                   gpointer user_data)
{
    WeatherApplet *applet = user_data;
    GtkWidget *results = g_object_get_data(G_OBJECT(button), "results");
    GtkWidget *status = g_object_get_data(G_OBJECT(button), "status");
    GtkWidget *saved_places = g_object_get_data(G_OBJECT(button),
                                                "saved-places");
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(results));
    Place *selected;
    guint i;

    if (!row)
    {
        gtk_label_set_text(GTK_LABEL(status), "Bitte zuerst einen Treffer auswählen.");
        return;
    }
    selected = g_object_get_data(G_OBJECT(row), "tux-place");
    if (!selected)
    {
        gtk_label_set_text(GTK_LABEL(status), "Der ausgewählte Ort ist ungültig.");
        return;
    }
    applet->auto_location = FALSE;
    for (i = 0; i < applet->places->len; i++)
    {
        Place *place = g_ptr_array_index(applet->places, i);
        if (fabs(place->latitude - selected->latitude) < 0.001 &&
            fabs(place->longitude - selected->longitude) < 0.001)
        {
            applet->place_index = i;
            populate_saved_places(applet, GTK_COMBO_BOX_TEXT(saved_places));
            save_settings(applet);
            clear_weather_for_place_change(applet);
            fetch_weather(applet);
            gtk_label_set_text(GTK_LABEL(status), "Dieser Ort ist bereits gespeichert.");
            return;
        }
    }
    g_ptr_array_add(applet->places,
                    place_new(selected->name, selected->latitude,
                              selected->longitude));
    applet->place_index = applet->places->len - 1;
    populate_saved_places(applet, GTK_COMBO_BOX_TEXT(saved_places));
    save_settings(applet);
    clear_weather_for_place_change(applet);
    fetch_weather(applet);
    gtk_label_set_text(GTK_LABEL(status), "Ort gespeichert.");
}

static void
remove_place(GtkButton *button,
             gpointer user_data)
{
    WeatherApplet *applet = user_data;
    GtkWidget *saved_places = g_object_get_data(G_OBJECT(button),
                                                "saved-places");
    GtkWidget *status = g_object_get_data(G_OBJECT(button), "status");
    gint remove_index = saved_places
        ? gtk_combo_box_get_active(GTK_COMBO_BOX(saved_places))
        : (gint) applet->place_index;

    if (applet->places->len <= 1)
    {
        if (status)
            gtk_label_set_text(GTK_LABEL(status),
                               "Mindestens ein Ort muss gespeichert bleiben.");
        return;
    }
    if (remove_index < 0 || (guint) remove_index >= applet->places->len)
        return;
    g_ptr_array_remove_index(applet->places, (guint) remove_index);
    if (applet->place_index > (guint) remove_index)
        applet->place_index--;
    else if (applet->place_index == (guint) remove_index)
        applet->place_index %= applet->places->len;
    if (saved_places)
        populate_saved_places(applet, GTK_COMBO_BOX_TEXT(saved_places));
    save_settings(applet);
    clear_weather_for_place_change(applet);
    fetch_weather(applet);
    if (status)
        gtk_label_set_text(GTK_LABEL(status), "Ort entfernt.");
}

static gboolean
import_settings_file(WeatherApplet *applet,
                     const gchar *path,
                     GError **error)
{
    GKeyFile *imported = g_key_file_new();
    gchar **saved_places;
    gsize n_places = 0;
    GPtrArray *places = g_ptr_array_new_with_free_func(place_free);
    Place *auto_place = NULL;
    gint saved_index = 0;
    guint i;

    if (!g_key_file_load_from_file(imported, path, G_KEY_FILE_NONE, error))
        goto failed;
    if (!g_key_file_has_group(imported, "display") ||
        !g_key_file_has_group(imported, "location") ||
        !g_key_file_has_key(imported, "location", PLACES_KEY, NULL))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Die Datei ist keine TuX-Wetter-Sicherung.");
        goto failed;
    }
    saved_places = g_key_file_get_string_list(imported, "location",
                                               PLACES_KEY, &n_places, error);
    if (!saved_places)
        goto failed;
    for (i = 0; i < n_places; i++)
    {
        gchar **fields = g_strsplit(saved_places[i], "|", 3);
        gchar *name = fields[0] ? g_uri_unescape_string(fields[0], NULL) : NULL;
        gchar *end_lat = NULL;
        gchar *end_lon = NULL;
        gdouble lat = fields[1] ? g_ascii_strtod(fields[1], &end_lat) : NAN;
        gdouble lon = fields[2] ? g_ascii_strtod(fields[2], &end_lon) : NAN;
        if (name && end_lat != fields[1] && *end_lat == '\0' &&
            end_lon != fields[2] && *end_lon == '\0' &&
            isfinite(lat) && isfinite(lon) &&
            lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)
            g_ptr_array_add(places, place_new(name, lat, lon));
        g_free(name);
        g_strfreev(fields);
    }
    g_strfreev(saved_places);
    if (places->len == 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Die Sicherung enthält keinen gültigen Ort.");
        goto failed;
    }

    saved_index = g_key_file_get_integer(imported, "location",
                                         "active-place-index", NULL);
    applet->place_index = saved_index >= 0 &&
        (guint) saved_index < places->len ? (guint) saved_index : 0;
    if (g_key_file_has_key(imported, "display", "fahrenheit", NULL))
        applet->fahrenheit = g_key_file_get_boolean(imported, "display",
                                                    "fahrenheit", NULL);
    if (g_key_file_has_key(imported, "display", "refresh-minutes", NULL))
    {
        gint refresh = g_key_file_get_integer(imported, "display",
                                              "refresh-minutes", NULL);
        if (refresh >= 10 && refresh <= 180)
            applet->refresh_minutes = (guint) refresh;
    }
    if (g_key_file_has_key(imported, "display", "theme", NULL))
    {
        gchar *theme = g_key_file_get_string(imported, "display",
                                              "theme", NULL);
        if (theme && *theme)
        {
            g_free(applet->theme);
            applet->theme = theme;
        }
        else
            g_free(theme);
    }
    if (g_key_file_has_key(imported, "provider", "weather-source", NULL))
    {
        gchar *provider = g_key_file_get_string(imported, "provider",
                                                "weather-source", NULL);
        if (g_strcmp0(provider, "openmeteo") == 0 ||
            g_strcmp0(provider, "metno") == 0 ||
            g_strcmp0(provider, "dwd") == 0)
        {
            g_free(applet->provider);
            applet->provider = g_strdup(provider);
        }
        g_free(provider);
    }
    if (g_key_file_has_key(imported, "display", "language", NULL))
    {
        gchar *language = g_key_file_get_string(imported, "display",
                                                "language", NULL);
        if (g_strcmp0(language, "auto") == 0 ||
            g_strcmp0(language, "de") == 0 ||
            g_strcmp0(language, "en") == 0)
        {
            g_free(applet->language);
            applet->language = language;
        }
        else
            g_free(language);
    }
#define IMPORT_BOOLEAN(group, key, field) \
    G_STMT_START { \
        if (g_key_file_has_key(imported, group, key, NULL)) \
            applet->field = g_key_file_get_boolean(imported, group, key, NULL); \
    } G_STMT_END
    IMPORT_BOOLEAN("display", "show-panel-icon", show_panel_icon);
    IMPORT_BOOLEAN("display", "show-panel-temperature", show_panel_temperature);
    IMPORT_BOOLEAN("display", "show-details", show_details);
    IMPORT_BOOLEAN("display", "show-forecast", show_forecast);
    IMPORT_BOOLEAN("display", "show-air-quality", show_air_quality);
    IMPORT_BOOLEAN("display", "show-moon", show_moon);
    IMPORT_BOOLEAN("display", "show-measured", show_measured);
    IMPORT_BOOLEAN("display", "animations", animations);
    IMPORT_BOOLEAN("location", "auto-enabled", auto_location);
    IMPORT_BOOLEAN("alerts", "enabled", warnings_enabled);
    IMPORT_BOOLEAN("alerts", "notifications", notifications_enabled);
#undef IMPORT_BOOLEAN
    if (g_key_file_has_key(imported, "alerts", "minimum-level", NULL))
        applet->notification_level = (guint) CLAMP(
            g_key_file_get_integer(imported, "alerts", "minimum-level", NULL),
            1, 4);
    if (g_key_file_has_key(imported, "location", "auto-name", NULL) &&
        g_key_file_has_key(imported, "location", "auto-latitude", NULL) &&
        g_key_file_has_key(imported, "location", "auto-longitude", NULL))
    {
        gchar *name = g_key_file_get_string(imported, "location",
                                             "auto-name", NULL);
        gdouble lat = g_key_file_get_double(imported, "location",
                                             "auto-latitude", NULL);
        gdouble lon = g_key_file_get_double(imported, "location",
                                             "auto-longitude", NULL);
        if (name && isfinite(lat) && isfinite(lon) &&
            lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180)
            auto_place = place_new(name, lat, lon);
        g_free(name);
    }
    g_ptr_array_unref(applet->places);
    applet->places = places;
    place_free(applet->auto_place);
    applet->auto_place = auto_place;
    g_key_file_unref(applet->settings);
    applet->settings = imported;
    save_settings(applet);
    apply_theme(applet);
    apply_display_options(applet);
    set_animation_state(applet);
    if (applet->refresh_source)
        g_source_remove(applet->refresh_source);
    applet->refresh_source = g_timeout_add_seconds(
        applet->refresh_minutes * 60, refresh_timeout, applet);
    clear_weather_for_place_change(applet);
    if (applet->auto_location)
        fetch_auto_location(applet);
    else
        fetch_weather(applet);
    return TRUE;

failed:
    g_ptr_array_unref(places);
    g_key_file_unref(imported);
    return FALSE;
}

static void
export_settings_clicked(GtkButton *button,
                        gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(user_data);
    WeatherApplet *applet = g_object_get_data(G_OBJECT(dialog), "applet");
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "TuX-Wetter-Einstellungen sichern", GTK_WINDOW(dialog),
        GTK_FILE_CHOOSER_ACTION_SAVE, "Abbrechen", GTK_RESPONSE_CANCEL,
        "Sichern", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "status");
    gint response;

    (void) button;
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser),
                                     "tux-wetter-settings.ini");
    response = gtk_dialog_run(GTK_DIALOG(chooser));
    if (response == GTK_RESPONSE_ACCEPT)
    {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        gsize length;
        GError *error = NULL;
        gchar *data = g_key_file_to_data(applet->settings, &length, &error);
        if (data && g_file_set_contents(path, data, (gssize) length, &error))
            gtk_label_set_text(GTK_LABEL(status),
                               "Einstellungen wurden gesichert.");
        else
        {
            gtk_label_set_text(GTK_LABEL(status), error
                               ? error->message : "Export fehlgeschlagen.");
            g_warning("%s: settings export failed: %s", APP_NAME,
                      error ? error->message : "unable to serialize settings");
        }
        g_clear_error(&error);
        g_free(data);
        g_free(path);
    }
    gtk_widget_destroy(chooser);
}

static void
import_settings_clicked(GtkButton *button,
                        gpointer user_data)
{
    GtkWidget *dialog = GTK_WIDGET(user_data);
    WeatherApplet *applet = g_object_get_data(G_OBJECT(dialog), "applet");
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "status");
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "TuX-Wetter-Einstellungen laden", GTK_WINDOW(dialog),
        GTK_FILE_CHOOSER_ACTION_OPEN, "Abbrechen", GTK_RESPONSE_CANCEL,
        "Laden", GTK_RESPONSE_ACCEPT, NULL);
    gint response;

    (void) button;
    response = gtk_dialog_run(GTK_DIALOG(chooser));
    if (response == GTK_RESPONSE_ACCEPT)
    {
        gchar *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        GError *error = NULL;
        if (import_settings_file(applet, path, &error))
        {
            gtk_widget_destroy(chooser);
            gtk_widget_destroy(dialog);
            gtk_label_set_text(GTK_LABEL(applet->status),
                               "Einstellungen importiert.");
            gtk_widget_show(applet->status);
            g_free(path);
            return;
        }
        gtk_label_set_text(GTK_LABEL(status), error
                           ? error->message : "Import fehlgeschlagen.");
        g_warning("%s: settings import failed: %s", APP_NAME,
                  error ? error->message : "invalid settings file");
        g_clear_error(&error);
        g_free(path);
    }
    gtk_widget_destroy(chooser);
}

static void
preferences_response(GtkDialog *dialog,
                     gint response,
                     gpointer user_data)
{
    WeatherApplet *applet = user_data;
    GtkWidget *unit = g_object_get_data(G_OBJECT(dialog), "unit");
    GtkWidget *theme = g_object_get_data(G_OBJECT(dialog), "theme");
    GtkWidget *refresh = g_object_get_data(G_OBJECT(dialog), "refresh");
    GtkWidget *panel_icon = g_object_get_data(G_OBJECT(dialog), "panel-icon");
    GtkWidget *panel_temperature = g_object_get_data(G_OBJECT(dialog),
                                                     "panel-temperature");
    GtkWidget *details = g_object_get_data(G_OBJECT(dialog), "details");
    GtkWidget *forecast = g_object_get_data(G_OBJECT(dialog), "forecast");
    GtkWidget *saved_places = g_object_get_data(G_OBJECT(dialog),
                                                "saved-places");
    GtkWidget *provider = g_object_get_data(G_OBJECT(dialog), "provider");
    GtkWidget *air_quality = g_object_get_data(G_OBJECT(dialog), "air-quality");
    GtkWidget *moon = g_object_get_data(G_OBJECT(dialog), "moon");
    GtkWidget *measured = g_object_get_data(G_OBJECT(dialog), "measured");
    GtkWidget *auto_location = g_object_get_data(G_OBJECT(dialog), "auto-location");
    GtkWidget *warnings = g_object_get_data(G_OBJECT(dialog), "warnings");
    GtkWidget *notifications = g_object_get_data(G_OBJECT(dialog), "notifications");
    GtkWidget *notify_level = g_object_get_data(G_OBJECT(dialog), "notify-level");
    GtkWidget *language = g_object_get_data(G_OBJECT(dialog), "language");
    GtkWidget *animations = g_object_get_data(G_OBJECT(dialog), "animations");
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "status");

    if (response != GTK_RESPONSE_CANCEL)
    {
        gchar *selected_theme;
        gchar *selected_provider;
        applet->fahrenheit = gtk_combo_box_get_active(GTK_COMBO_BOX(unit)) == 1;
        applet->refresh_minutes = (guint) gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(refresh));
        applet->show_panel_icon =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(panel_icon));
        applet->show_panel_temperature =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(panel_temperature));
        applet->show_details =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(details));
        applet->show_forecast =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(forecast));
        applet->show_air_quality =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(air_quality));
        applet->show_moon =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(moon));
        applet->show_measured =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(measured));
        applet->animations =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(animations));
        applet->auto_location =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(auto_location));
        applet->warnings_enabled =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(warnings));
        applet->notifications_enabled =
            gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(notifications));
        applet->notification_level = (guint) gtk_spin_button_get_value_as_int(
            GTK_SPIN_BUTTON(notify_level));
        {
            gchar *selected_language = g_strdup(gtk_combo_box_get_active_id(
                GTK_COMBO_BOX(language)));
            if (selected_language)
            {
                g_free(applet->language);
                applet->language = selected_language;
            }
        }
        selected_provider = gtk_combo_box_text_get_active_text(
            GTK_COMBO_BOX_TEXT(provider));
        if (selected_provider)
        {
            g_free(applet->provider);
            applet->provider = selected_provider;
        }
        if (gtk_combo_box_get_active(GTK_COMBO_BOX(saved_places)) >= 0)
            applet->place_index = (guint) gtk_combo_box_get_active(
                GTK_COMBO_BOX(saved_places));
        selected_theme = gtk_combo_box_text_get_active_text(
            GTK_COMBO_BOX_TEXT(theme));
        g_free(applet->theme);
        applet->theme = selected_theme;
        if (!applet->theme)
            applet->theme = g_strdup("violett");
        apply_theme(applet);
        apply_display_options(applet);
        set_animation_state(applet);
        save_settings(applet);
        update_display(applet);
        if (applet->refresh_source)
            g_source_remove(applet->refresh_source);
        applet->refresh_source = g_timeout_add_seconds(
            applet->refresh_minutes * 60, refresh_timeout, applet);
        if (applet->auto_location)
            fetch_auto_location(applet);
        else
            fetch_weather(applet);
    }
    (void) status;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
show_preferences(WeatherApplet *applet)
{
    GtkWidget *dialog;
    GtkWidget *content;
    GtkWidget *grid;
    GtkWidget *entry;
    GtkWidget *search;
    GtkWidget *results;
    GtkWidget *scroll;
    GtkWidget *status;
    GtkWidget *add;
    GtkWidget *remove;
    GtkWidget *unit;
    GtkWidget *theme;
    GtkWidget *refresh;
    GtkWidget *label;
    GtkWidget *search_row;
    GtkWidget *place_section;
    GtkWidget *panel_icon;
    GtkWidget *panel_temperature;
    GtkWidget *details;
    GtkWidget *forecast;
    GtkWidget *saved_places;
    GtkWidget *provider;
    GtkWidget *air_quality;
    GtkWidget *moon;
    GtkWidget *measured;
    GtkWidget *auto_location;
    GtkWidget *warnings;
    GtkWidget *notifications;
    GtkWidget *notify_level;
    GtkWidget *prefs_scroll;
    GtkWidget *language;
    GtkWidget *animations;
    GtkWidget *export_settings;
    GtkWidget *import_settings;
    gint unit_index;
    gint theme_index = 0;
    guint i;
    static const gchar *themes[] = {
        "violett", "blau", "petrol", "gruen", "bordeaux",
        "sonnenuntergang", "graphit", "nacht"
    };

    dialog = gtk_dialog_new_with_buttons("TuX-Wetter einrichten",
                                         GTK_WINDOW(gtk_widget_get_toplevel(
                                             GTK_WIDGET(applet->plugin))),
                                         GTK_DIALOG_DESTROY_WITH_PARENT,
                                         "Schließen", GTK_RESPONSE_CLOSE,
                                         "Speichern und schließen", GTK_RESPONSE_OK,
                                         NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 900);
    content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    prefs_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(prefs_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(content), prefs_scroll, TRUE, TRUE, 0);
    grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_container_set_border_width(GTK_CONTAINER(grid), 14);
    gtk_container_add(GTK_CONTAINER(prefs_scroll), grid);

    place_section = make_label("Ort suchen und speichern", "heading");
    gtk_widget_set_halign(place_section, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), place_section, 0, 0, 3, 1);
    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Ort oder Postleitzahl");
    search = gtk_button_new_with_label("Suchen");
    search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(search_row), entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(search_row), search, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), search_row, 0, 1, 3, 1);

    results = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(results), GTK_SELECTION_SINGLE);
    scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 130);
    gtk_container_add(GTK_CONTAINER(scroll), results);
    gtk_grid_attach(GTK_GRID(grid), scroll, 0, 2, 3, 1);

    status = gtk_label_new("Die Ortsdaten werden an Open-Meteo gesendet.");
    gtk_widget_set_halign(status, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
    gtk_grid_attach(GTK_GRID(grid), status, 0, 3, 3, 1);
    label = make_label("Gespeicherte Orte", NULL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 4, 3, 1);
    saved_places = gtk_combo_box_text_new();
    populate_saved_places(applet, GTK_COMBO_BOX_TEXT(saved_places));
    gtk_grid_attach(GTK_GRID(grid), saved_places, 0, 5, 3, 1);
    add = gtk_button_new_with_label("Ausgewählten Ort hinzufügen");
    remove = gtk_button_new_with_label("Ausgewählten Ort entfernen");
    gtk_grid_attach(GTK_GRID(grid), add, 0, 6, 2, 1);
    gtk_grid_attach(GTK_GRID(grid), remove, 2, 6, 1, 1);

    label = make_label("Einheit", NULL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 7, 1, 1);
    unit = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(unit), "Celsius");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(unit), "Fahrenheit");
    unit_index = applet->fahrenheit ? 1 : 0;
    gtk_combo_box_set_active(GTK_COMBO_BOX(unit), unit_index);
    gtk_grid_attach(GTK_GRID(grid), unit, 1, 7, 2, 1);

    label = make_label("Farbschema", NULL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 8, 1, 1);
    theme = gtk_combo_box_text_new();
    for (i = 0; i < G_N_ELEMENTS(themes); i++)
    {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(theme), themes[i]);
        if (g_strcmp0(applet->theme, themes[i]) == 0)
            theme_index = (gint) i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(theme), theme_index);
    gtk_grid_attach(GTK_GRID(grid), theme, 1, 8, 2, 1);

    label = make_label("Aktualisierung (Minuten)", NULL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 9, 1, 1);
    refresh = gtk_spin_button_new_with_range(10, 180, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(refresh),
                              applet->refresh_minutes);
    gtk_grid_attach(GTK_GRID(grid), refresh, 1, 9, 2, 1);

    label = make_label("Panelanzeige und Wetterdetails", "heading");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 10, 3, 1);
    panel_icon = gtk_check_button_new_with_label("Wetter-Symbol im Panel");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(panel_icon),
                                 applet->show_panel_icon);
    gtk_grid_attach(GTK_GRID(grid), panel_icon, 0, 11, 3, 1);
    panel_temperature = gtk_check_button_new_with_label(
        "Temperatur im Panel");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(panel_temperature),
                                 applet->show_panel_temperature);
    gtk_grid_attach(GTK_GRID(grid), panel_temperature, 0, 12, 3, 1);
    details = gtk_check_button_new_with_label(
        "Wetterdetails (Wind, Luftfeuchte, UV und Niederschlag)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(details),
                                 applet->show_details);
    gtk_grid_attach(GTK_GRID(grid), details, 0, 13, 3, 1);
    forecast = gtk_check_button_new_with_label(
        "Stunden- und 7-Tage-Vorschau anzeigen");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(forecast),
                                 applet->show_forecast);
    gtk_grid_attach(GTK_GRID(grid), forecast, 0, 14, 3, 1);
    label = make_label("Wetteranbieter für aktuelle Messwerte", NULL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 15, 1, 1);
    provider = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(provider), "openmeteo",
                              "Open-Meteo");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(provider), "metno",
                              "MET Norway (yr.no)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(provider), "dwd",
                              "DWD über Bright Sky");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(provider), applet->provider);
    gtk_grid_attach(GTK_GRID(grid), provider, 1, 15, 2, 1);
    label = make_label("Open-Meteo liefert weiterhin Vorhersage, UV und Sonnenzeiten.", "weather-small");
    gtk_grid_attach(GTK_GRID(grid), label, 0, 16, 3, 1);
    air_quality = gtk_check_button_new_with_label(
        "Luftqualität sowie Birken-/Gräserpollen");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(air_quality),
                                 applet->show_air_quality);
    gtk_grid_attach(GTK_GRID(grid), air_quality, 0, 17, 3, 1);
    moon = gtk_check_button_new_with_label(
        "Mondphase und Mondauf/-untergang");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(moon), applet->show_moon);
    gtk_grid_attach(GTK_GRID(grid), moon, 0, 18, 3, 1);
    measured = gtk_check_button_new_with_label(
        "DWD-Messstationswert anzeigen (bei DWD-Anbieter)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(measured),
                                 applet->show_measured);
    gtk_grid_attach(GTK_GRID(grid), measured, 0, 19, 3, 1);
    auto_location = gtk_check_button_new_with_label(
        "Standort automatisch über GeoJS bestimmen (sendet IP-Standortdaten)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(auto_location),
                                 applet->auto_location);
    gtk_grid_attach(GTK_GRID(grid), auto_location, 0, 20, 3, 1);
    warnings = gtk_check_button_new_with_label(
        "Amtliche Wetterwarnungen für den Standort anzeigen (CH/AT/DE)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(warnings),
                                 applet->warnings_enabled);
    gtk_grid_attach(GTK_GRID(grid), warnings, 0, 21, 3, 1);
    notifications = gtk_check_button_new_with_label(
        "Desktop-Benachrichtigungen für Warnungen aktivieren");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(notifications),
                                 applet->notifications_enabled);
    gtk_grid_attach(GTK_GRID(grid), notifications, 0, 22, 3, 1);
    label = make_label("Mindestwarnstufe für Benachrichtigungen (1–4)", NULL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 23, 2, 1);
    notify_level = gtk_spin_button_new_with_range(1, 4, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(notify_level),
                              applet->notification_level);
    gtk_grid_attach(GTK_GRID(grid), notify_level, 2, 23, 1, 1);
    label = make_label("Sprache / Sprache des Systems", NULL);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 24, 2, 1);
    language = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(language), "auto", "System");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(language), "de", "Deutsch");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(language), "en", "English");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(language), applet->language);
    gtk_grid_attach(GTK_GRID(grid), language, 2, 24, 1, 1);
    label = make_label("Einstellungen sichern oder laden", "heading");
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), label, 0, 25, 3, 1);
    export_settings = gtk_button_new_with_label("Sicherung exportieren …");
    import_settings = gtk_button_new_with_label("Sicherung importieren …");
    gtk_grid_attach(GTK_GRID(grid), export_settings, 0, 26, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), import_settings, 1, 26, 2, 1);
    animations = gtk_check_button_new_with_label(
        "Sanfte Symbolanimation anzeigen");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(animations),
                                 applet->animations);
    gtk_grid_attach(GTK_GRID(grid), animations, 0, 27, 3, 1);

    gtk_widget_set_halign(status, GTK_ALIGN_START);
    g_object_set_data(G_OBJECT(dialog), "unit", unit);
    g_object_set_data(G_OBJECT(dialog), "theme", theme);
    g_object_set_data(G_OBJECT(dialog), "refresh", refresh);
    g_object_set_data(G_OBJECT(dialog), "panel-icon", panel_icon);
    g_object_set_data(G_OBJECT(dialog), "panel-temperature", panel_temperature);
    g_object_set_data(G_OBJECT(dialog), "details", details);
    g_object_set_data(G_OBJECT(dialog), "forecast", forecast);
    g_object_set_data(G_OBJECT(dialog), "saved-places", saved_places);
    g_object_set_data(G_OBJECT(dialog), "provider", provider);
    g_object_set_data(G_OBJECT(dialog), "air-quality", air_quality);
    g_object_set_data(G_OBJECT(dialog), "moon", moon);
    g_object_set_data(G_OBJECT(dialog), "measured", measured);
    g_object_set_data(G_OBJECT(dialog), "auto-location", auto_location);
    g_object_set_data(G_OBJECT(dialog), "warnings", warnings);
    g_object_set_data(G_OBJECT(dialog), "notifications", notifications);
    g_object_set_data(G_OBJECT(dialog), "notify-level", notify_level);
    g_object_set_data(G_OBJECT(dialog), "language", language);
    g_object_set_data(G_OBJECT(dialog), "animations", animations);
    g_object_set_data(G_OBJECT(dialog), "applet", applet);
    g_object_set_data(G_OBJECT(dialog), "status", status);
    g_object_set_data(G_OBJECT(search), "dialog", dialog);
    g_object_set_data(G_OBJECT(search), "entry", entry);
    g_object_set_data(G_OBJECT(search), "results", results);
    g_object_set_data(G_OBJECT(search), "status", status);
    g_object_set_data(G_OBJECT(add), "dialog", dialog);
    g_object_set_data(G_OBJECT(add), "results", results);
    g_object_set_data(G_OBJECT(add), "status", status);
    g_object_set_data(G_OBJECT(add), "saved-places", saved_places);
    g_object_set_data(G_OBJECT(remove), "status", status);
    g_object_set_data(G_OBJECT(remove), "saved-places", saved_places);
    g_signal_connect(search, "clicked", G_CALLBACK(search_places), applet);
    g_signal_connect(add, "clicked", G_CALLBACK(add_selected_place), applet);
    g_signal_connect(remove, "clicked", G_CALLBACK(remove_place), applet);
    g_signal_connect(unit, "changed", G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(theme, "changed", G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(refresh, "value-changed",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(panel_icon, "toggled",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(panel_temperature, "toggled",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(details, "toggled", G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(forecast, "toggled",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(saved_places, "changed",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(provider, "changed", G_CALLBACK(preferences_changed),
                     dialog);
    g_signal_connect(air_quality, "toggled",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(moon, "toggled", G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(measured, "toggled",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(auto_location, "toggled",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(warnings, "toggled",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(notifications, "toggled",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(notify_level, "value-changed",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(language, "changed", G_CALLBACK(preferences_changed),
                     dialog);
    g_signal_connect(export_settings, "clicked",
                     G_CALLBACK(export_settings_clicked), dialog);
    g_signal_connect(import_settings, "clicked",
                     G_CALLBACK(import_settings_clicked), dialog);
    g_signal_connect(animations, "toggled",
                     G_CALLBACK(preferences_changed), dialog);
    g_signal_connect(dialog, "response",
                     G_CALLBACK(preferences_response), applet);
    gtk_widget_show_all(dialog);
}

static void
configure_plugin(XfcePanelPlugin *plugin,
                 gpointer user_data)
{
    (void) plugin;
    show_preferences(user_data);
}

static void
orientation_changed(XfcePanelPlugin *plugin,
                   GtkOrientation orientation,
                   gpointer user_data)
{
    WeatherApplet *applet = user_data;
    (void) plugin;
    gtk_orientable_set_orientation(GTK_ORIENTABLE(applet->panel_box),
                                   orientation);
}

static void
applet_free(gpointer data)
{
    WeatherApplet *applet = data;

    if (applet->weather_window)
        gtk_widget_destroy(applet->weather_window);
    if (applet->refresh_source)
        g_source_remove(applet->refresh_source);
    g_clear_object(&applet->session);
    if (applet->settings)
        g_key_file_unref(applet->settings);
    if (applet->places)
        g_ptr_array_unref(applet->places);
    place_free(applet->auto_place);
    weather_data_free(applet->weather);
    g_free(applet->settings_path);
    g_free(applet->theme);
    g_free(applet->provider);
    g_free(applet->language);
    g_free(applet->official_warning);
    g_free(applet->last_notification);
    g_free(applet);
}

static void
construct_plugin(XfcePanelPlugin *plugin)
{
    WeatherApplet *applet = g_new0(WeatherApplet, 1);
    applet->plugin = plugin;
    load_settings(applet);
    applet->session = soup_session_new();
    soup_session_set_timeout(applet->session, 15);
    g_object_set_data_full(G_OBJECT(plugin), "tux-weather-applet",
                           applet, applet_free);

    applet->button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(applet->button), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(applet->button, APP_NAME);
    applet->panel_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    applet->panel_image = gtk_image_new_from_icon_name("weather-few-clouds",
                                                       GTK_ICON_SIZE_MENU);
    applet->panel_temperature = gtk_label_new("--");
    gtk_box_pack_start(GTK_BOX(applet->panel_box), applet->panel_image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(applet->panel_box), applet->panel_temperature, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(applet->button), applet->panel_box);
    gtk_container_add(GTK_CONTAINER(plugin), applet->button);
    xfce_panel_plugin_add_action_widget(plugin, applet->button);
    g_signal_connect(plugin, "configure-plugin",
                     G_CALLBACK(configure_plugin), applet);
    xfce_panel_plugin_menu_show_configure(plugin);
    g_signal_connect(plugin, "orientation-changed",
                     G_CALLBACK(orientation_changed), applet);
    g_signal_connect(plugin, "size-changed",
                     G_CALLBACK(panel_size_changed), applet);
    orientation_changed(plugin, xfce_panel_plugin_get_orientation(plugin),
                        applet);
    panel_size_changed(plugin, xfce_panel_plugin_get_size(plugin), applet);

    build_weather_window(applet);
    g_signal_connect(applet->weather_window, "map",
                     G_CALLBACK(popup_mapped), applet);
    g_signal_connect(applet->weather_window, "unmap",
                     G_CALLBACK(popup_unmapped), applet);
    g_signal_connect(applet->button, "clicked",
                     G_CALLBACK(toggle_weather_window), applet);
    gtk_widget_show_all(applet->button);
    gtk_widget_hide(applet->warning);
    gtk_widget_hide(applet->status);
    apply_display_options(applet);

    applet->refresh_source = g_timeout_add_seconds(
        applet->refresh_minutes * 60, refresh_timeout, applet);
    fetch_weather(applet);
    if (applet->auto_location)
        fetch_auto_location(applet);
}

XFCE_PANEL_PLUGIN_REGISTER(construct_plugin)
