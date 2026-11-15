/*
 * SPDX-FileCopyrightText: 2026 R.Bruns
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef TUX_WETTER_WEATHER_H
#define TUX_WETTER_WEATHER_H

#include <glib.h>

typedef struct
{
    gchar *time;
    gdouble temperature;
    gdouble precipitation_mm;
    gint code;
    gint precipitation_probability;
} WeatherHour;

typedef struct
{
    gchar *date;
    gdouble minimum;
    gdouble maximum;
    gint code;
    gint precipitation_probability;
    gdouble precipitation_mm;
    gdouble snowfall_cm;
    WeatherHour chart[8];
    guint n_chart;
} WeatherDay;

typedef struct
{
    gchar *timezone;
    gchar *observed_at;
    gdouble temperature;
    gdouble apparent_temperature;
    gdouble wind_speed;
    gdouble wind_gusts;
    gdouble wind_direction;
    gdouble humidity;
    gdouble precipitation_mm;
    gdouble precipitation_next_2h_mm;
    gdouble precipitation_probability;
    gdouble uv_index;
    gdouble european_aqi;
    gdouble birch_pollen;
    gdouble grass_pollen;
    gdouble alder_pollen;
    gdouble mugwort_pollen;
    gdouble ragweed_pollen;
    gdouble olive_pollen;
    gdouble moon_phase;
    gdouble measured_temperature;
    gdouble measured_distance_km;
    gint utc_offset_seconds;
    gdouble today_minimum;
    gdouble today_maximum;
    gint weather_code;
    gboolean is_day;
    gchar *sunrise;
    gchar *sunset;
    gchar *moonrise;
    gchar *moonset;
    gchar *station_name;
    gchar *provider_name;
    WeatherHour hours[6];
    guint n_hours;
    WeatherDay days[7];
    guint n_days;
} WeatherData;

WeatherData *weather_data_parse(const gchar *json, gsize length, GError **error);
gboolean weather_data_apply_metno_current(const gchar *json,
                                          gsize length,
                                          WeatherData *weather,
                                          GError **error);
gboolean weather_data_apply_brightsky_current(const gchar *json,
                                              gsize length,
                                              WeatherData *weather,
                                              GError **error);
gchar *weather_response_error(const gchar *json, gsize length, guint status);
gchar *weather_coordinate_to_query(gdouble coordinate);
void weather_data_free(WeatherData *weather);
const gchar *weather_description(gint code, gboolean is_day);
const gchar *weather_description_english(gint code, gboolean is_day);
const gchar *weather_icon_name(gint code, gboolean is_day);

#endif
