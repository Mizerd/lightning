#pragma once

#include <QDateTime>
#include <QString>

namespace lightning::portable {

inline constexpr char kMarkerFileName[] = "portable.marker";

// The portable data directory name, here so lightning-updater (Qt6::Core only)
// can name it without linking this unit. The updater must exclude it from the
// directory swap or an update destroys the session and crypto store.
inline constexpr char kDataDirName[] = "data";

inline constexpr char kPortableEnvVar[] = "LIGHTNING_PORTABLE";

QString executableDir();

bool isPortable();

QString dataRoot();

QString configDir();
QString secretsDir();
QString cacheDir();
QString logsDir();
QString tempDir();
QString mediaScratchRoot();
void setMediaScratchRootOverrideForTest(const QString &root);
void holdScratchDirLive(const QString &dir);
void releaseScratchDir(const QString &dir);
inline constexpr char kScratchLiveLockName[] = ".lightning-live.lock";

int cleanStaleTempDirs();
int cleanStaleTempDirs(const QDateTime &now);
QString updateWorkDir();

bool markerPresentIn(const QString &dir);
QString dataRootFor(const QString &executableDir);

void setPortableOverrideForTest(bool portable, const QString &executableDir);
void clearPortableOverrideForTest();

QString prepareDataRoot();

}
