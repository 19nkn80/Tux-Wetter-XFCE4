#include "../src/plugin.c"

static void
test_provider_settings(void)
{
    const gchar *ids[] = { "openmeteo", "metno", "dwd" };
    const gchar *old_labels[] = {
        "Open-Meteo", "MET Norway (yr.no)", "DWD über Bright Sky"
    };
    guint i;

    for (i = 0; i < G_N_ELEMENTS(ids); i++)
    {
        WeatherApplet *applet = g_new0(WeatherApplet, 1);
        GtkWidget *combo;

        load_settings(applet);
        combo = provider_combo_new(applet->provider);
        g_object_ref_sink(combo);
        g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), ids[i]));
        set_provider_from_combo(applet, GTK_COMBO_BOX(combo));
        g_assert_cmpstr(applet->provider, ==, ids[i]);
        save_settings(applet);
        gtk_widget_destroy(combo);
        g_object_unref(combo);
        applet_free(applet);

        applet = g_new0(WeatherApplet, 1);
        load_settings(applet);
        combo = provider_combo_new(applet->provider);
        g_object_ref_sink(combo);
        g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)), ==,
                        ids[i]);
        gtk_widget_destroy(combo);
        g_object_unref(combo);

        g_free(applet->provider);
        applet->provider = g_strdup(old_labels[i]);
        save_settings(applet);
        applet_free(applet);
        applet = g_new0(WeatherApplet, 1);
        load_settings(applet);
        g_assert_cmpstr(applet->provider, ==, ids[i]);
        applet_free(applet);
    }
}

int
main(int argc, char **argv)
{
    GError *error = NULL;
    gchar *directory = g_dir_make_tmp("tux-wetter-test-XXXXXX", &error);
    gchar *settings;
    gchar *backup;
    gchar *subdirectory;
    int result;

    if (!directory)
        g_error("Cannot create test directory: %s", error->message);
    g_setenv("XDG_CONFIG_HOME", directory, TRUE);
    g_test_init(&argc, &argv, NULL);
    if (!gtk_init_check(&argc, &argv))
    {
        g_test_message("Skipping provider widget test: no display");
        g_rmdir(directory);
        g_free(directory);
        return 0;
    }
    g_test_add_func("/preferences/provider-persistence", test_provider_settings);
    result = g_test_run();
    subdirectory = g_build_filename(directory, "tux-wetter-xfce", NULL);
    settings = g_build_filename(subdirectory, "settings.ini", NULL);
    backup = g_strconcat(settings, ".bak", NULL);
    g_remove(backup);
    g_remove(settings);
    g_rmdir(subdirectory);
    g_rmdir(directory);
    g_free(backup);
    g_free(settings);
    g_free(subdirectory);
    g_free(directory);
    return result;
}
