/* Loads each named element's plugin, like `gst-inspect-1.0 <element>`, but
 * without gst_macos_main: its NSApplication never starts on a headless Mac,
 * so gst-inspect-1.0 hangs there before gst_init. Exit 0 when all load. */
#include <gst/gst.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    GError *error = NULL;
    if (!gst_init_check(&argc, &argv, &error)) {
        fprintf(stderr, "gst_init failed: %s\n", error ? error->message : "unknown");
        return 2;
    }
    int missing = 0;
    for (int i = 1; i < argc; i++) {
        GstElementFactory *factory = gst_element_factory_find(argv[i]);
        GstPluginFeature *loaded = factory ? gst_plugin_feature_load(GST_PLUGIN_FEATURE(factory)) : NULL;
        if (!loaded) {
            fprintf(stderr, "%s: no such element or its plugin did not load\n", argv[i]);
            missing++;
        } else {
            gst_object_unref(loaded);
        }
        if (factory)
            gst_object_unref(factory);
    }
    return missing ? 1 : 0;
}
