// ONE GStreamer initialisation for the whole process, and the bundled-plugin
// path that has to be set before it.
//
// WHY THIS EXISTS AS ITS OWN UNIT. There are two media backends — the 1:1
// `GstCallMediaBackend` and the SFU `SfuMediaEngine` — and each used to call
// `gst_init_check()` from its own `std::call_once`. That is harmless on a
// machine whose GStreamer is already on the default plugin path, and fatal on
// a PACKAGED build:
//
//   * `GST_PLUGIN_PATH` is read DURING `gst_init`, once.
//   * `AppController::enableCallMediaEngine()` probes the 1:1 backend FIRST.
//   * So the first `gst_init` ran with no plugin path, scanned the builder's
//     sysroot directory (which does not exist on a user's machine), and
//     registered nothing. The SFU engine then set `GST_PLUGIN_PATH` and called
//     `gst_init_check()` again — a NO-OP, because GStreamer was already
//     initialised — and every element probe failed.
//
// The visible result on Windows was a client that had the engine compiled in
// and 25 plugins beside it and still reported `missing_element:webrtcbin`:
// no call button, and an incoming call offering only Decline and Dismiss.
//
// The fix is not "set the path in both places" — that is the same bug waiting
// for a third caller. It is ONE entry point that does the path and the init
// together, so the order in which the backends are probed cannot matter.
#pragma once

#include <QString>

namespace lightning::gst {

/// Initialise GStreamer exactly once, with the bundled plugin path applied
/// first. Safe to call from any backend, in any order, any number of times.
///
/// Returns false only when `gst_init` itself failed; `whyNot` then carries a
/// sanitized category, never a path.
bool ensureInitialised(QString *whyNot = nullptr);

/// The runtime GStreamer version, e.g. "GStreamer 1.28.5". Empty before
/// ensureInitialised() has run.
///
/// Worth reporting because the receive path depends on what webrtcbin fills
/// in on a src pad, and that has moved between releases: this is developed
/// against 1.26.x and shipped against 1.28.x on both Windows and macOS.
QString versionString();

/// The directory the bundled plugins were taken from, or empty when this is a
/// development build using the system GStreamer. Diagnostics only.
QString bundledPluginPath();

/// Which bundled plugin directory an AppImage is ALREADY using, or empty.
///
/// The AppImage is the one Linux package that carries its own GStreamer, and
/// it does not put it beside the binary: linuxdeploy's layout is
/// `usr/bin/lightning-matrix` with the plugins in `usr/lib/gstreamer-1.0`, and
/// the AppRun hook exports `GST_PLUGIN_SYSTEM_PATH_1_0` and
/// `GST_PLUGIN_PATH_1_0` at them before this process starts. So there is
/// nothing for us to SET; what was missing is that nothing NOTICED, and
/// `--call-media-status` reported "using system GStreamer" on the one Linux
/// package that does not use one.
///
/// That is worth fixing because the flag exists so a tester's output
/// identifies their runtime without a round trip, and four separate packaging
/// defects in this project have turned on exactly which GStreamer was loaded.
///
/// It answers what is TRUE rather than what the layout suggests: one of the
/// two variables the hook sets must actually name the directory. A bundle
/// whose hook did not run is not reported as bundled, because it is not being
/// used. Pure, and deliberately does NOT touch the filesystem, so the rule is
/// testable without an AppImage; the caller checks the directory exists.
QString appImageBundledPluginPath(const QString &appDir,
                                  const QString &systemPath,
                                  const QString &pluginPath);

} // namespace lightning::gst

