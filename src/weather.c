/*
 * SPDX-FileCopyrightText: 2026 R.Bruns
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "weather.h"

#include <json-glib/json-glib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static gdouble
node_number(JsonNode *node,
            gdouble fallback)
{
    GType type;
    if (!node || JSON_NODE_HOLDS_NULL(node) || !JSON_NODE_HOLDS_VALUE(node))
        return fallback;
    type = json_node_get_value_type(node);
    if (type == G_TYPE_DOUBLE)
        return json_node_get_double(node);
    if (type == G_TYPE_INT64 || type == G_TYPE_INT)
        return (gdouble) json_node_get_int(node);
    return fallback;
}

gchar *
weather_coordinate_to_query(gdouble coordinate)
{
    gchar buffer[G_ASCII_DTOSTR_BUF_SIZE];

    g_ascii_formatd(buffer, sizeof(buffer), "%.6f", coordinate);
    return g_strdup(buffer);
}

static gdouble
object_number(JsonObject *object,
              const gchar *name,
              gdouble fallback)
{
    return object && json_object_has_member(object, name)
        ? node_number(json_object_get_member(object, name), fallback)
        : fallback;
}

static const gchar *
object_string(JsonObject *object,
              const gchar *name)
{
    JsonNode *node;

    if (!object || !json_object_has_member(object, name))
        return NULL;
    node = json_object_get_member(object, name);
    if (JSON_NODE_HOLDS_VALUE(node) &&
        json_node_get_value_type(node) == G_TYPE_STRING)
        return json_node_get_string(node);
    return NULL;
}

static gint
array_int(JsonArray *array,
          guint index,
          gint fallback)
{
    JsonNode *node = json_array_get_element(array, index);
    gdouble value = node_number(node, NAN);
    return isfinite(value) ? (gint) value : fallback;
}

static gdouble
array_double(JsonArray *array,
             guint index,
             gdouble fallback)
{
    JsonNode *node = json_array_get_element(array, index);

    return node_number(node, fallback);
}

static gchar *
array_string_dup(JsonArray *array,
                 guint index)
{
    JsonNode *node = json_array_get_element(array, index);

    if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;
    return g_strdup(json_node_get_string(node));
}

static JsonArray *
object_array(JsonObject *object,
             const gchar *name)
{
    JsonNode *node;

    if (!object || !json_object_has_member(object, name))
        return NULL;
    node = json_object_get_member(object, name);
    return JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

WeatherData *
weather_data_parse(const gchar *json,
                   gsize length,
                   GError **error)
{
    JsonParser *parser;
    JsonNode *root;
    JsonObject *root_object;
    JsonObject *current;
    JsonObject *daily;
    JsonObject *hourly;
    JsonArray *times;
    JsonArray *temperatures;
    JsonArray *codes;
    JsonArray *probabilities;
    JsonArray *array;
    WeatherData *weather;
    guint i;

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, (gssize) length, error))
    {
        g_object_unref(parser);
        return NULL;
    }

    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Open-Meteo returned a non-object response");
        g_object_unref(parser);
        return NULL;
    }
    root_object = json_node_get_object(root);
    if (!json_object_has_member(root_object, "current") ||
        !json_object_has_member(root_object, "daily") ||
        !json_object_has_member(root_object, "hourly") ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(root_object, "current")) ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(root_object, "daily")) ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(root_object, "hourly")))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Open-Meteo response is missing forecast data");
        g_object_unref(parser);
        return NULL;
    }

    current = json_object_get_object_member(root_object, "current");
    daily = json_object_get_object_member(root_object, "daily");
    hourly = json_object_get_object_member(root_object, "hourly");
    weather = g_new0(WeatherData, 1);
    weather->timezone = g_strdup(object_string(root_object, "timezone"));
    weather->utc_offset_seconds = (gint) object_number(root_object,
                                                        "utc_offset_seconds", 0);
    weather->observed_at = g_strdup(object_string(current, "time"));
    weather->temperature = object_number(current, "temperature_2m", NAN);
    weather->apparent_temperature = object_number(current, "apparent_temperature", NAN);
    weather->wind_speed = object_number(current, "wind_speed_10m", NAN);
    weather->wind_gusts = object_number(current, "wind_gusts_10m", NAN);
    weather->wind_direction = object_number(current, "wind_direction_10m", NAN);
    weather->humidity = object_number(current, "relative_humidity_2m", NAN);
    weather->precipitation_mm = object_number(current, "precipitation", NAN);
    weather->precipitation_next_2h_mm = NAN;
    weather->weather_code = (gint) object_number(current, "weather_code", -1);
    weather->is_day = object_number(current, "is_day", 1) != 0;
    weather->precipitation_probability = NAN;

    array = object_array(daily, "temperature_2m_min");
    weather->today_minimum = array && json_array_get_length(array) > 0
        ? array_double(array, 0, NAN) : NAN;
    array = object_array(daily, "temperature_2m_max");
    weather->today_maximum = array && json_array_get_length(array) > 0
        ? array_double(array, 0, NAN) : NAN;
    array = object_array(daily, "uv_index_max");
    weather->uv_index = array && json_array_get_length(array) > 0
        ? array_double(array, 0, NAN) : NAN;
    weather->european_aqi = NAN;
    weather->birch_pollen = NAN;
    weather->grass_pollen = NAN;
    weather->alder_pollen = NAN;
    weather->mugwort_pollen = NAN;
    weather->ragweed_pollen = NAN;
    weather->olive_pollen = NAN;
    weather->moon_phase = NAN;
    weather->measured_temperature = NAN;
    weather->measured_distance_km = NAN;
    array = object_array(daily, "sunrise");
    weather->sunrise = array && json_array_get_length(array) > 0
        ? array_string_dup(array, 0) : NULL;
    array = object_array(daily, "sunset");
    weather->sunset = array && json_array_get_length(array) > 0
        ? array_string_dup(array, 0) : NULL;

    times = object_array(hourly, "time");
    temperatures = object_array(hourly, "temperature_2m");
    codes = object_array(hourly, "weather_code");
    probabilities = object_array(hourly, "precipitation_probability");
    {
        JsonArray *rain = object_array(hourly, "precipitation");
        JsonArray *gusts = object_array(hourly, "wind_gusts_10m");
        guint limit = gusts ? MIN(json_array_get_length(gusts), 24) : 0;
        for (i = 0; i < limit; i++)
        {
            gdouble gust = array_double(gusts, i, NAN);
            if (isfinite(gust) &&
                (!isfinite(weather->wind_gusts) || gust > weather->wind_gusts))
                weather->wind_gusts = gust;
        }
        if (rain && json_array_get_length(rain) > 0)
        {
            weather->precipitation_mm = array_double(rain, 0,
                                                       weather->precipitation_mm);
            weather->precipitation_next_2h_mm = 0;
            for (i = 0; i < MIN(json_array_get_length(rain), 2); i++)
            {
                gdouble amount = array_double(rain, i, 0);
                if (isfinite(amount))
                    weather->precipitation_next_2h_mm += amount;
            }
        }
    }
    if (times && temperatures && codes)
    {
        guint n = MIN(json_array_get_length(times),
                      MIN(json_array_get_length(temperatures),
                          json_array_get_length(codes)));
        gint first = 0;
        const gchar *now = weather->observed_at;

        if (now)
        {
            for (i = 0; i < n; i++)
            {
                gchar *hour_time = array_string_dup(times, i);
                gboolean is_current = hour_time && strcmp(hour_time, now) >= 0;
                g_free(hour_time);
                if (is_current)
                {
                    first = (gint) i;
                    break;
                }
            }
        }

        for (i = (guint) first; i < n && weather->n_hours < G_N_ELEMENTS(weather->hours); i++)
        {
            WeatherHour *hour = &weather->hours[weather->n_hours++];
            hour->time = array_string_dup(times, i);
            hour->temperature = array_double(temperatures, i, NAN);
            {
                JsonArray *rain = object_array(hourly, "precipitation");
                hour->precipitation_mm = rain &&
                    i < json_array_get_length(rain)
                    ? array_double(rain, i, NAN) : NAN;
            }
            hour->code = array_int(codes, i, -1);
            hour->precipitation_probability =
                probabilities && i < json_array_get_length(probabilities)
                ? array_int(probabilities, i, -1) : -1;
        }
    }

    times = object_array(daily, "time");
    temperatures = object_array(daily, "temperature_2m_min");
    codes = object_array(daily, "weather_code");
    array = object_array(daily, "temperature_2m_max");
    probabilities = object_array(daily, "precipitation_probability_max");
    weather->precipitation_probability =
        probabilities && json_array_get_length(probabilities) > 0
        ? array_double(probabilities, 0, NAN) : NAN;
    if (times && temperatures && codes && array)
    {
        guint n = MIN(json_array_get_length(times),
                      MIN(json_array_get_length(temperatures),
                          MIN(json_array_get_length(codes),
                              json_array_get_length(array))));
        JsonArray *precipitation = object_array(daily, "precipitation_sum");
        JsonArray *snowfall = object_array(daily, "snowfall_sum");

        for (i = 0; i < n && weather->n_days < G_N_ELEMENTS(weather->days); i++)
        {
            WeatherDay *day = &weather->days[weather->n_days++];
            day->date = array_string_dup(times, i);
            day->minimum = array_double(temperatures, i, NAN);
            day->maximum = array_double(array, i, NAN);
            day->code = array_int(codes, i, -1);
            day->precipitation_probability =
                probabilities && i < json_array_get_length(probabilities)
                ? array_int(probabilities, i, -1) : -1;
            day->precipitation_mm = precipitation &&
                i < json_array_get_length(precipitation)
                ? array_double(precipitation, i, NAN) : NAN;
            day->snowfall_cm = snowfall &&
                i < json_array_get_length(snowfall)
                ? array_double(snowfall, i, NAN) : NAN;
        }
    }
    {
        JsonArray *chart_times = object_array(hourly, "time");
        JsonArray *chart_temperatures = object_array(hourly, "temperature_2m");
        JsonArray *chart_codes = object_array(hourly, "weather_code");
        JsonArray *chart_rain = object_array(hourly, "precipitation");
        JsonArray *chart_probability =
            object_array(hourly, "precipitation_probability");
        if (chart_times && chart_temperatures && chart_codes)
        {
            guint n = MIN(json_array_get_length(chart_times),
                          MIN(json_array_get_length(chart_temperatures),
                              json_array_get_length(chart_codes)));
            guint day_index;
            for (day_index = 0; day_index < weather->n_days; day_index++)
            {
                WeatherDay *day = &weather->days[day_index];
                guint hour_index;
                for (hour_index = 0; hour_index < n; hour_index++)
                {
                    gchar *time = array_string_dup(chart_times, hour_index);
                    gint local_hour = -1;
                    gboolean belongs_to_day = time && day->date &&
                        strncmp(time, day->date, 10) == 0;
                    if (belongs_to_day && strlen(time) >= 13)
                        sscanf(time + 11, "%2d", &local_hour);
                    if (belongs_to_day && local_hour >= 6 &&
                        local_hour <= 21 && local_hour % 3 == 0 &&
                        day->n_chart < G_N_ELEMENTS(day->chart))
                    {
                        WeatherHour *point = &day->chart[day->n_chart++];
                        point->time = time;
                        point->temperature =
                            array_double(chart_temperatures, hour_index, NAN);
                        point->code = array_int(chart_codes, hour_index, -1);
                        point->precipitation_mm = chart_rain &&
                            hour_index < json_array_get_length(chart_rain)
                            ? array_double(chart_rain, hour_index, NAN) : NAN;
                        point->precipitation_probability = chart_probability &&
                            hour_index < json_array_get_length(chart_probability)
                            ? array_int(chart_probability, hour_index, -1) : -1;
                    }
                    else
                        g_free(time);
                }
            }
        }
    }

    g_object_unref(parser);
    if (!isfinite(weather->temperature) || weather->n_days == 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Open-Meteo response contains no usable weather values");
        weather_data_free(weather);
        return NULL;
    }
    return weather;
}

gchar *
weather_response_error(const gchar *json,
                       gsize length,
                       guint status)
{
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    gchar *message = NULL;

    if (json_parser_load_from_data(parser, json, (gssize) length, &error))
    {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root))
        {
            JsonObject *object = json_node_get_object(root);
            const gchar *reason = object_string(object, "reason");
            if (reason && *reason)
                message = g_strdup(reason);
        }
    }
    g_clear_error(&error);
    g_object_unref(parser);
    if (!message)
        message = g_strdup_printf("Wetterdienst antwortet mit HTTP %u", status);
    return message;
}

void
weather_data_free(WeatherData *weather)
{
    guint i;

    if (!weather)
        return;
    g_free(weather->timezone);
    g_free(weather->observed_at);
    g_free(weather->sunrise);
    g_free(weather->sunset);
    g_free(weather->moonrise);
    g_free(weather->moonset);
    g_free(weather->station_name);
    g_free(weather->provider_name);
    for (i = 0; i < weather->n_hours; i++)
        g_free(weather->hours[i].time);
    for (i = 0; i < weather->n_days; i++)
    {
        g_free(weather->days[i].date);
        guint j;
        for (j = 0; j < weather->days[i].n_chart; j++)
            g_free(weather->days[i].chart[j].time);
    }
    g_free(weather);
}

static JsonObject *
parse_object(const gchar *json,
             gsize length,
             JsonParser **parser,
             GError **error)
{
    *parser = json_parser_new();
    if (!json_parser_load_from_data(*parser, json, (gssize) length, error))
    {
        g_clear_object(parser);
        return NULL;
    }
    if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(*parser)))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Weather provider returned a non-object response");
        g_clear_object(parser);
        return NULL;
    }
    return json_node_get_object(json_parser_get_root(*parser));
}

static gint
weather_code_from_icon(const gchar *icon)
{
    if (!icon)
        return 3;
    if (strstr(icon, "thunderstorm") || strstr(icon, "thunder"))
        return 95;
    if (strstr(icon, "hail"))
        return 96;
    if (g_str_has_prefix(icon, "lightsnowshowers"))
        return 85;
    if (g_str_has_prefix(icon, "heavysnowshowers"))
        return 86;
    if (g_str_has_prefix(icon, "heavysnow"))
        return 75;
    if (g_str_has_prefix(icon, "lightsnow"))
        return 71;
    if (strstr(icon, "snow"))
        return 73;
    if (strstr(icon, "sleet"))
        return 68;
    if (g_str_has_prefix(icon, "heavyrainshowers"))
        return 82;
    if (g_str_has_prefix(icon, "lightrainshowers"))
        return 80;
    if (g_str_has_prefix(icon, "rainshowers"))
        return 81;
    if (g_str_has_prefix(icon, "heavyrain"))
        return 65;
    if (g_str_has_prefix(icon, "lightrain"))
        return 61;
    if (strstr(icon, "rain"))
        return 63;
    if (strstr(icon, "drizzle"))
        return 51;
    if (strstr(icon, "fog"))
        return 45;
    if (strstr(icon, "partly-cloudy") || strstr(icon, "partlycloudy"))
        return 2;
    if (g_str_has_prefix(icon, "fair"))
        return 1;
    if (strstr(icon, "cloudy"))
        return 3;
    if (strstr(icon, "clear") || strstr(icon, "sun"))
        return 0;
    return 3;
}

gboolean
weather_data_apply_metno_current(const gchar *json,
                                 gsize length,
                                 WeatherData *weather,
                                 GError **error)
{
    JsonParser *parser;
    JsonObject *root = parse_object(json, length, &parser, error);
    JsonObject *properties;
    JsonArray *timeseries;
    JsonObject *entry;
    JsonObject *data;
    JsonObject *instant;
    JsonObject *details;
    JsonObject *next;
    JsonObject *summary;
    const gchar *symbol;

    if (!root)
        return FALSE;
    properties = json_object_get_object_member(root, "properties");
    timeseries = properties
        ? json_object_get_array_member(properties, "timeseries") : NULL;
    if (!timeseries || json_array_get_length(timeseries) == 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "MET Norway response contains no forecast values");
        g_object_unref(parser);
        return FALSE;
    }
    entry = json_array_get_object_element(timeseries, 0);
    data = entry ? json_object_get_object_member(entry, "data") : NULL;
    instant = data ? json_object_get_object_member(data, "instant") : NULL;
    details = instant ? json_object_get_object_member(instant, "details") : NULL;
    next = data ? json_object_get_object_member(data, "next_1_hours") : NULL;
    summary = next ? json_object_get_object_member(next, "summary") : NULL;
    symbol = object_string(summary, "symbol_code");
    if (!details || !object_string(entry, "time"))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "MET Norway response is missing current conditions");
        g_object_unref(parser);
        return FALSE;
    }
    weather->temperature = object_number(details, "air_temperature", NAN);
    weather->apparent_temperature = object_number(details,
        "apparent_air_temperature", weather->temperature);
    weather->wind_speed = object_number(details, "wind_speed", NAN) * 3.6;
    weather->wind_gusts = object_number(details, "wind_speed_of_gust", NAN) * 3.6;
    weather->wind_direction = object_number(details, "wind_from_direction", NAN);
    weather->humidity = object_number(details, "relative_humidity", NAN);
    weather->weather_code = weather_code_from_icon(symbol);
    weather->is_day = !symbol || !strstr(symbol, "night");
    g_free(weather->observed_at);
    weather->observed_at = g_strdup(object_string(entry, "time"));
    g_free(weather->provider_name);
    weather->provider_name = g_strdup("MET Norway");
    g_object_unref(parser);
    return isfinite(weather->temperature);
}

gboolean
weather_data_apply_brightsky_current(const gchar *json,
                                     gsize length,
                                     WeatherData *weather,
                                     GError **error)
{
    JsonParser *parser;
    JsonObject *root = parse_object(json, length, &parser, error);
    JsonObject *current;
    JsonArray *sources;
    const gchar *icon;
    const gchar *timestamp;
    gdouble temperature;
    gdouble dewpoint;
    gdouble humidity;
    gdouble source_id;
    GDateTime *observed;
    GDateTime *now;
    gint64 age_seconds;

    if (!root)
        return FALSE;
    current = json_object_get_object_member(root, "weather");
    if (!current)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Bright Sky response contains no current weather");
        g_object_unref(parser);
        return FALSE;
    }
    temperature = object_number(current, "temperature", NAN);
    if (!isfinite(temperature))
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Bright Sky response contains no temperature");
        g_object_unref(parser);
        return FALSE;
    }
    icon = object_string(current, "icon");
    timestamp = object_string(current, "timestamp");
    weather->temperature = temperature;
    weather->weather_code = weather_code_from_icon(icon);
    weather->is_day = !icon || !strstr(icon, "night");
    weather->wind_speed = object_number(current, "wind_speed", NAN);
    weather->wind_gusts = object_number(current, "wind_gust_speed", NAN);
    weather->wind_direction = object_number(current, "wind_direction", NAN);
    humidity = object_number(current, "relative_humidity", NAN);
    dewpoint = object_number(current, "dew_point", NAN);
    if (isfinite(humidity))
        weather->humidity = humidity;
    else if (isfinite(dewpoint))
    {
        gdouble saturation_temperature =
            exp(17.625 * temperature / (243.04 + temperature));
        gdouble saturation_dewpoint =
            exp(17.625 * dewpoint / (243.04 + dewpoint));
        weather->humidity = CLAMP(100.0 * saturation_dewpoint /
                                  saturation_temperature, 0.0, 100.0);
    }
    if (isfinite(dewpoint) && isfinite(weather->humidity))
    {
        gdouble vapor = weather->humidity / 100.0 *
            6.105 * exp(17.27 * temperature / (237.7 + temperature));
        weather->apparent_temperature = temperature + 0.33 * vapor -
            0.70 * weather->wind_speed / 3.6 - 4.0;
    }
    sources = json_object_get_array_member(root, "sources");
    observed = timestamp ? g_date_time_new_from_iso8601(timestamp, NULL) : NULL;
    now = g_date_time_new_now_utc();
    age_seconds = observed
        ? g_date_time_to_unix(now) - g_date_time_to_unix(observed)
        : G_MAXINT64;
    if (observed)
        g_date_time_unref(observed);
    g_date_time_unref(now);
    source_id = object_number(current, "source_id", NAN);
    if (sources && json_array_get_length(sources) > 0 &&
        age_seconds >= 0 && age_seconds <= 75 * 60)
    {
        guint i;
        for (i = 0; i < json_array_get_length(sources); i++)
        {
            JsonObject *source = json_array_get_object_element(sources, i);
            gdouble distance;
            if (!source ||
                (isfinite(source_id) &&
                 object_number(source, "id", -1) != source_id))
                continue;
            distance = object_number(source, "distance", NAN) / 1000.0;
            if (!isfinite(distance) || distance > 25)
                continue;
            g_free(weather->station_name);
            weather->station_name = g_strdup(object_string(source,
                                                            "station_name"));
            weather->measured_temperature = temperature;
            weather->measured_distance_km = distance;
            break;
        }
    }
    g_free(weather->provider_name);
    weather->provider_name = g_strdup("DWD via Bright Sky");
    g_object_unref(parser);
    return TRUE;
}

const gchar *
weather_description(gint code,
                    gboolean is_day)
{
    switch (code)
    {
    case 0: return is_day ? "Sonnig" : "Klar";
    case 1: return is_day ? "Überwiegend sonnig" : "Überwiegend klar";
    case 2: return "Teilweise bewölkt";
    case 3: return "Bedeckt";
    case 45: case 48: return "Nebel";
    case 51: case 53: case 55: return "Nieselregen";
    case 56: case 57: return "Gefrierender Niesel";
    case 61: return "Leichter Regen";
    case 63: return "Regen";
    case 65: return "Starker Regen";
    case 66: case 67: return "Gefrierender Regen";
    case 68: return "Schneeregen";
    case 71: return "Leichter Schneefall";
    case 73: return "Schneefall";
    case 75: return "Starker Schneefall";
    case 77: return "Schneegriesel";
    case 80: return "Leichte Regenschauer";
    case 81: return "Regenschauer";
    case 82: return "Heftige Regenschauer";
    case 85: case 86: return "Schneeschauer";
    case 95: return "Gewitter";
    case 96: case 99: return "Gewitter mit Hagel";
    default: return "Unbekannt";
    }
}

const gchar *
weather_icon_name(gint code,
                  gboolean is_day)
{
    if (code == 0)
        return is_day ? "weather-clear" : "weather-clear-night";
    if (code <= 2)
        return is_day ? "weather-few-clouds" : "weather-few-clouds-night";
    if (code == 3)
        return "weather-overcast";
    if (code == 45 || code == 48)
        return "weather-fog";
    if (code >= 51 && code <= 57)
        return "weather-showers-scattered";
    if ((code >= 61 && code <= 68) || (code >= 80 && code <= 82))
        return "weather-showers";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86)
        return "weather-snow";
    if (code >= 95)
        return "weather-storm";
    return "weather-overcast";
}

const gchar *
weather_description_english(gint code,
                            gboolean is_day)
{
    switch (code)
    {
    case 0: return is_day ? "Sunny" : "Clear";
    case 1: return is_day ? "Mainly sunny" : "Mainly clear";
    case 2: return "Partly cloudy";
    case 3: return "Overcast";
    case 45: case 48: return "Fog";
    case 51: case 53: case 55: return "Drizzle";
    case 56: case 57: return "Freezing drizzle";
    case 61: return "Light rain";
    case 63: return "Rain";
    case 65: return "Heavy rain";
    case 66: case 67: return "Freezing rain";
    case 68: return "Sleet";
    case 71: return "Light snow";
    case 73: return "Snow";
    case 75: return "Heavy snow";
    case 77: return "Snow grains";
    case 80: return "Light showers";
    case 81: return "Rain showers";
    case 82: return "Heavy showers";
    case 85: case 86: return "Snow showers";
    case 95: return "Thunderstorm";
    case 96: case 99: return "Thunderstorm with hail";
    default: return "Unknown";
    }
}
