// One GStreamer initialisation for the whole process, with the bundled plugin
// path set before it.
//
// GST_PLUGIN_PATH is read once, during gst_init. When the 1:1 backend and the
// SFU engine each initialised GStreamer themselves, whichever was probed
// first decided whether the bundled path was applied, and packaged builds
// could end up with an empty registry (`missing_element:webrtcbin`). A single
// entry point makes probe order irrelevant.
#pragma once

#include <QString>

namespace lightning::gst {

/// Initialise GStreamer exactly once, with the bundled plugin path applied
/// first. Safe from any backend, in any order, any number of times. Returns
/// false only when gst_init failed; `whyNot` then carries a sanitized
/// category, never a path.
bool ensureInitialised(QString *whyNot = nullptr);

/// The runtime GStreamer version, e.g. "GStreamer 1.28.5"; empty before
/// ensureInitialised(). Reported because webrtcbin behaviour differs between
/// the versions we develop and ship against.
QString versionString();

/// The directory the bundled plugins came from, or empty for a system
/// GStreamer. Diagnostics only.
QString bundledPluginPath();

/// Where a bundled `gst-plugin-scanner` would live, given the executable's
/// directory; empty for empty input.
///
/// GStreamer loads candidate plugins in a separate helper process so a
/// crashing plugin cannot take the app down. The helper's path is compiled in
/// as the builder's libexec directory, which does not exist in a relocated
/// bundle, so GStreamer warns ("External plugin loader failed") and scans
/// in-process. That works, but loses crash isolation and adds a misleading
/// warning to every log.
///
/// On macOS the helper sits beside the executable in `Contents/MacOS`, the one
/// bundle directory meant for executables. Pure (no filesystem access) so it
/// is testable anywhere.
QString scannerPathBesideExecutable(const QString &applicationDirPath);

/// Point GStreamer at the scanner beside `applicationDirPath`, unless an
/// override is already set. Returns true only when this call set the
/// variables; false covers an existing override, no scanner, or a
/// non-executable file, all of which leave GStreamer's in-process fallback.
/// Only the call site is Apple-guarded, so the logic is testable everywhere.
bool applyBundledScannerPath(const QString &applicationDirPath);

/// The scanner this process pointed GStreamer at, or empty (development
/// build, existing override, or none bundled). Diagnostics only.
QString bundledScannerPath();

/// The bundled plugin directory an AppImage is already using, or empty.
///
/// The AppImage ships GStreamer in `usr/lib/gstreamer-1.0` and its AppRun
/// hook exports `GST_PLUGIN_SYSTEM_PATH_1_0` and `GST_PLUGIN_PATH_1_0` before
/// we start, so nothing needs setting; this lets `--call-media-status` report
/// it. Answers what is true: one of the two variables must name the
/// directory, so a bundle whose hook did not run is not reported. Pure; the
/// caller checks the directory exists.
QString appImageBundledPluginPath(const QString &appDir,
                                  const QString &systemPath,
                                  const QString &pluginPath);

/// The plugin scanner an AppImage is already using, or empty.
///
/// The AppRun hook exports `GST_PLUGIN_SCANNER_1_0` (the helper lives in
/// `usr/libexec/gstreamer-1.0/`, not beside the binary), so nothing needs
/// setting; this only records it for `--call-media-status`. Other Linux
/// packages use a system or runtime GStreamer whose compiled-in path is
/// correct and must be left alone, so one of the variables must actually name
/// the expected location under `appDir`. Pure; the caller checks the file
/// exists and is executable.
QString appImageBundledScannerPath(const QString &appDir,
                                   const QString &scannerVersioned,
                                   const QString &scannerPlain);

} // namespace lightning::gst

