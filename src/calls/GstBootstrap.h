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

bool ensureInitialised(QString *whyNot = nullptr);

QString versionString();

QString bundledPluginPath();

QString scannerPathBesideExecutable(const QString &applicationDirPath);

bool applyBundledScannerPath(const QString &applicationDirPath);

QString bundledScannerPath();

QString appImageBundledPluginPath(const QString &appDir,
                                  const QString &systemPath,
                                  const QString &pluginPath);

QString appImageBundledScannerPath(const QString &appDir,
                                   const QString &scannerVersioned,
                                   const QString &scannerPlain);

}

