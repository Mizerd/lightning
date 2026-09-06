/*
 * gst-element-probe -- does a packaged Lightning tree really carry a working
 * GStreamer?
 *
 * The Windows packages bundle GStreamer plugins in `gstreamer-1.0/` beside the
 * executable, and a plugin is dlopen'd rather than linked, so no import table
 * names one and no file listing proves one LOADS. This probe reproduces exactly
 * what SfuMediaEngine::runtimeAvailable() does -- point GST_PLUGIN_PATH at the
 * bundled directory, clear GST_PLUGIN_SYSTEM_PATH, gst_init, then ask the
 * registry for each element by name -- and exits non-zero if any is missing.
 *
 * It is built into the packaging image and is never shipped in a package.
 */
#include <gst/gst.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: gst-element-probe ELEMENT...\n");
        return 2;
    }

    gchar *exe = g_win32_get_package_installation_directory_of_module(NULL);
    if (!exe) {
        fprintf(stderr, "gst-element-probe: cannot resolve the executable directory\n");
        return 2;
    }
    gchar *bundled = g_build_filename(exe, "gstreamer-1.0", NULL);
    if (g_file_test(bundled, G_FILE_TEST_IS_DIR)) {
        g_setenv("GST_PLUGIN_PATH", bundled, TRUE);
        g_setenv("GST_PLUGIN_SYSTEM_PATH", "", TRUE);
    } else {
        fprintf(stderr, "gst-element-probe: no bundled plugin directory at %s\n", bundled);
        return 2;
    }
    printf("plugin path: %s\n", bundled);

    GError *error = NULL;
    if (!gst_init_check(NULL, NULL, &error)) {
        fprintf(stderr, "gst-element-probe: gst_init failed: %s\n",
                error ? error->message : "unknown");
        return 2;
    }

    int missing = 0;
    for (int i = 1; i < argc; ++i) {
        GstElementFactory *factory = gst_element_factory_find(argv[i]);
        if (factory) {
            printf("ok      %s\n", argv[i]);
            gst_object_unref(factory);
        } else {
            printf("MISSING %s\n", argv[i]);
            ++missing;
        }
    }
    printf("gst-element-probe: %d requested, %d missing\n", argc - 1, missing);
    return missing == 0 ? 0 : 1;
}
