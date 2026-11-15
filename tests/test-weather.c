/*
 * SPDX-FileCopyrightText: 2026 R.Bruns
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "../src/weather.h"

#include <gio/gio.h>
#include <math.h>
#include <locale.h>
#include <string.h>

static const gchar response[] =
    "{"
    "\"timezone\":\"Europe/Zurich\","
    "\"current\":{\"time\":\"2026-11-15T06:00\",\"temperature_2m\":12.5,"
    "\"apparent_temperature\":11.0,\"relative_humidity_2m\":72,"
    "\"weather_code\":61,\"is_day\":1,\"wind_speed_10m\":8,"
    "\"wind_gusts_10m\":10,\"wind_direction_10m\":225,\"precipitation\":0.2},"
    "\"hourly\":{\"time\":[\"2026-11-15T06:00\",\"2026-11-15T09:00\","
    "\"2026-11-15T12:00\"],\"temperature_2m\":[12.5,13.2,14.0],"
    "\"weather_code\":[61,3,2],\"precipitation_probability\":[40,20,10],"
    "\"precipitation\":[0.2,0.0,0.0],\"wind_gusts_10m\":[10,51,24]},"
    "\"daily\":{\"time\":[\"2026-11-15\",\"2026-11-16\"],"
    "\"weather_code\":[61,3],\"temperature_2m_min\":[8,7],"
    "\"temperature_2m_max\":[14,15],\"precipitation_probability_max\":[65,10],"
    "\"precipitation_sum\":[2.4,0.0],\"snowfall_sum\":[0,0],"
    "\"sunrise\":[\"2026-11-15T07:10\",\"2026-11-16T07:12\"],"
    "\"sunset\":[\"2026-11-15T17:00\",\"2026-11-16T16:59\"],"
    "\"uv_index_max\":[2,2]}"
    "}";

static void
test_metno_current_conditions(void)
{
    const gchar metno[] =
        "{\"properties\":{\"timeseries\":[{\"time\":\"2026-11-15T10:00:00Z\","
        "\"data\":{\"instant\":{\"details\":{\"air_temperature\":8.5,"
        "\"relative_humidity\":81,\"wind_speed\":3.0,"
        "\"wind_from_direction\":240,\"wind_speed_of_gust\":5.0}},"
        "\"next_1_hours\":{\"summary\":{\"symbol_code\":\"rain\"}}}}]}}";
    GError *error = NULL;
    WeatherData *weather = weather_data_parse(response, strlen(response), &error);

    g_assert_no_error(error);
    g_assert_true(weather_data_apply_metno_current(metno, strlen(metno),
                                                   weather, &error));
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(weather->temperature, 8.5, 0.001);
    g_assert_cmpfloat_with_epsilon(weather->wind_speed, 10.8, 0.001);
    g_assert_cmpint(weather->weather_code, ==, 63);
    g_assert_cmpstr(weather->provider_name, ==, "MET Norway");
    weather_data_free(weather);
}

static void
test_brightsky_current_conditions(void)
{
    GDateTime *now = g_date_time_new_now_utc();
    gchar *timestamp = g_date_time_format_iso8601(now);
    gchar *brightsky = g_strdup_printf(
        "{\"weather\":{\"temperature\":9.0,\"condition\":\"rain\","
        "\"icon\":\"rain\",\"wind_speed\":12,\"wind_direction\":180,"
        "\"wind_gust_speed\":25,\"relative_humidity\":80,\"dew_point\":5,"
        "\"source_id\":3,\"timestamp\":\"%s\"},"
        "\"sources\":[{\"id\":3,\"station_name\":\"Luzern\","
        "\"distance\":4200}]}", timestamp);
    GError *error = NULL;
    WeatherData *weather = weather_data_parse(response, strlen(response), &error);

    g_assert_no_error(error);
    g_assert_true(weather_data_apply_brightsky_current(
        brightsky, strlen(brightsky), weather, &error));
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(weather->temperature, 9.0, 0.001);
    g_assert_cmpfloat_with_epsilon(weather->measured_distance_km, 4.2, 0.001);
    g_assert_cmpstr(weather->station_name, ==, "Luzern");
    g_assert_cmpint(weather->weather_code, ==, 63);
    g_assert_cmpstr(weather->provider_name, ==, "DWD via Bright Sky");
    weather_data_free(weather);
    g_free(brightsky);
    g_free(timestamp);
    g_date_time_unref(now);
}

static void
test_stale_brightsky_is_not_a_station_measurement(void)
{
    GDateTime *now = g_date_time_new_now_utc();
    GDateTime *old = g_date_time_add_minutes(now, -120);
    gchar *timestamp = g_date_time_format_iso8601(old);
    gchar *brightsky = g_strdup_printf(
        "{\"weather\":{\"temperature\":9.0,\"icon\":\"rain\","
        "\"wind_speed\":12,\"relative_humidity\":80,\"dew_point\":5,"
        "\"source_id\":3,\"timestamp\":\"%s\"},"
        "\"sources\":[{\"id\":3,\"station_name\":\"Luzern\","
        "\"distance\":4200}]}", timestamp);
    GError *error = NULL;
    WeatherData *weather = weather_data_parse(response, strlen(response), &error);

    g_assert_no_error(error);
    g_assert_true(weather_data_apply_brightsky_current(
        brightsky, strlen(brightsky), weather, &error));
    g_assert_no_error(error);
    g_assert_true(isnan(weather->measured_temperature));
    g_assert_null(weather->station_name);
    weather_data_free(weather);
    g_free(brightsky);
    g_free(timestamp);
    g_date_time_unref(old);
    g_date_time_unref(now);
}

static void
test_parse_forecast(void)
{
    GError *error = NULL;
    WeatherData *weather = weather_data_parse(response, strlen(response), &error);

    g_assert_no_error(error);
    g_assert_nonnull(weather);
    g_assert_cmpstr(weather->timezone, ==, "Europe/Zurich");
    g_assert_cmpfloat_with_epsilon(weather->temperature, 12.5, 0.001);
    g_assert_cmpint(weather->weather_code, ==, 61);
    g_assert_cmpint(weather->n_hours, ==, 3);
    g_assert_cmpint(weather->n_days, ==, 2);
    g_assert_cmpint(weather->days[0].n_chart, ==, 3);
    g_assert_cmpfloat_with_epsilon(weather->days[0].chart[1].temperature, 13.2, 0.001);
    g_assert_cmpfloat_with_epsilon(weather->precipitation_probability, 65, 0.001);
    g_assert_cmpfloat_with_epsilon(weather->wind_gusts, 51, 0.001);
    g_assert_cmpfloat_with_epsilon(weather->days[0].precipitation_mm, 2.4, 0.001);
    g_assert_cmpstr(weather->sunrise, ==, "2026-11-15T07:10");
    weather_data_free(weather);
}

static void
test_rejects_invalid_response(void)
{
    GError *error = NULL;
    WeatherData *weather = weather_data_parse("{\"error\":\"bad request\"}",
                                               strlen("{\"error\":\"bad request\"}"),
                                               &error);

    g_assert_null(weather);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void
test_extracts_service_error(void)
{
    const gchar response[] =
        "{\"error\":true,\"reason\":\"Invalid latitude\"}";
    gchar *message = weather_response_error(response, strlen(response), 400);

    g_assert_cmpstr(message, ==, "Invalid latitude");
    g_free(message);
}

static void
test_coordinate_query_uses_ascii_decimal(void)
{
    const gchar *current_locale = setlocale(LC_NUMERIC, NULL);
    gchar *saved_locale = g_strdup(current_locale);
    gchar *coordinate;

    if (!setlocale(LC_NUMERIC, "de_DE.UTF-8"))
    {
        g_test_skip("de_DE.UTF-8 locale is not installed");
        g_free(saved_locale);
        return;
    }

    coordinate = weather_coordinate_to_query(52.02);
    g_assert_cmpstr(coordinate, ==, "52.020000");
    g_free(coordinate);
    setlocale(LC_NUMERIC, saved_locale);
    g_free(saved_locale);
}

static void
test_weather_symbols(void)
{
    g_assert_cmpstr(weather_description(68, TRUE), ==, "Schneeregen");
    g_assert_cmpstr(weather_description_english(95, TRUE), ==, "Thunderstorm");
    g_assert_cmpstr(weather_icon_name(95, TRUE), ==, "weather-storm");
}

int
main(int argc,
     char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/weather/parse-forecast", test_parse_forecast);
    g_test_add_func("/weather/metno-current", test_metno_current_conditions);
    g_test_add_func("/weather/brightsky-current", test_brightsky_current_conditions);
    g_test_add_func("/weather/brightsky-stale-measurement",
                    test_stale_brightsky_is_not_a_station_measurement);
    g_test_add_func("/weather/reject-invalid", test_rejects_invalid_response);
    g_test_add_func("/weather/service-error", test_extracts_service_error);
    g_test_add_func("/weather/coordinate-ascii", test_coordinate_query_uses_ascii_decimal);
    g_test_add_func("/weather/symbols", test_weather_symbols);
    return g_test_run();
}
