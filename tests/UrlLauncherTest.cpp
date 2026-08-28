#include <QtTest>

#include "app/UrlLauncher.h"

// THE ENVIRONMENT A SPAWNED BROWSER GETS.
//
// QDesktopServices::openUrl() spawns xdg-open, and a spawned child inherits
// this process's environment. Inside an AppImage that environment points at the
// bundle — LD_LIBRARY_PATH puts $APPDIR/usr/lib first so Lightning's own Qt and
// GStreamer resolve — so the browser loads the bundle's glib/gio ahead of the
// host's and never starts. Reported against the 0.8.1 AppImage as "clicking
// links doesn't open them in browser", with no error surfaced anywhere.
//
// Asserted on the environment rather than by launching a browser: what a
// browser does with it is not observable in a test, and the environment IS the
// defect.
class UrlLauncherTest : public QObject
{
    Q_OBJECT
private:
    struct EnvGuard {
        QStringList keys;
        ~EnvGuard() { for (const QString &k : keys) qunsetenv(k.toLocal8Bit()); }
        void set(const char *k, const char *v)
        {
            keys << QString::fromLatin1(k);
            qputenv(k, QByteArray(v));
        }
    };

private Q_SLOTS:
    // Outside an AppImage nothing may be stripped: a normal install must
    // behave exactly as every previous release did.
    void outsideAnAppImageTheEnvironmentIsUntouched()
    {
        qunsetenv("APPDIR");
        EnvGuard guard;
        guard.set("LD_LIBRARY_PATH", "/host/lib");
        guard.set("GST_PLUGIN_SYSTEM_PATH_1_0", "/host/gst");
        const QProcessEnvironment env = lightning::urls::childEnvironment();
        QCOMPARE(env.value(QStringLiteral("LD_LIBRARY_PATH")),
                 QStringLiteral("/host/lib"));
        QCOMPARE(env.value(QStringLiteral("GST_PLUGIN_SYSTEM_PATH_1_0")),
                 QStringLiteral("/host/gst"));
    }

    void insideAnAppImageTheLoaderPathIsRestored()
    {
        EnvGuard guard;
        guard.set("APPDIR", "/tmp/.mount_xyz");
        guard.set("LD_LIBRARY_PATH", "/tmp/.mount_xyz/usr/lib");
        guard.set("APPIMAGE_ORIGINAL_LD_LIBRARY_PATH", "/host/lib");
        const QProcessEnvironment env = lightning::urls::childEnvironment();
        QCOMPARE(env.value(QStringLiteral("LD_LIBRARY_PATH")),
                 QStringLiteral("/host/lib"));
    }

    // The session had no LD_LIBRARY_PATH: the variable must be REMOVED, not set
    // to "". An empty entry is read by the loader as the current directory,
    // which is both wrong and a place an attacker may be able to write.
    void anEmptyOriginalRemovesTheVariableRatherThanEmptyingIt()
    {
        EnvGuard guard;
        guard.set("APPDIR", "/tmp/.mount_xyz");
        guard.set("LD_LIBRARY_PATH", "/tmp/.mount_xyz/usr/lib");
        guard.set("APPIMAGE_ORIGINAL_LD_LIBRARY_PATH", "");
        const QProcessEnvironment env = lightning::urls::childEnvironment();
        QVERIFY2(!env.contains(QStringLiteral("LD_LIBRARY_PATH")),
                 "an empty LD_LIBRARY_PATH entry means the current directory");
    }

    // Every variable that points into the mount must go: the mount does not
    // outlive Lightning, so a child holding one is looking at a path that will
    // vanish.
    void bundleOnlyVariablesAreStripped()
    {
        EnvGuard guard;
        guard.set("APPDIR", "/tmp/.mount_xyz");
        guard.set("APPIMAGE_ORIGINAL_LD_LIBRARY_PATH", "/host/lib");
        for (const char *k : { "GST_PLUGIN_SYSTEM_PATH_1_0", "GST_PLUGIN_PATH_1_0",
                               "GST_REGISTRY_1_0", "SPA_PLUGIN_DIR",
                               "PIPEWIRE_MODULE_DIR", "PIPEWIRE_CONFIG_DIR",
                               "QT_PLUGIN_PATH", "QML2_IMPORT_PATH" })
            guard.set(k, "/tmp/.mount_xyz/usr/lib/whatever");

        const QProcessEnvironment env = lightning::urls::childEnvironment();
        for (const QString &k : guard.keys) {
            if (k == QLatin1String("APPDIR")
                || k == QLatin1String("APPIMAGE_ORIGINAL_LD_LIBRARY_PATH"))
                continue;
            QVERIFY2(!env.contains(k),
                     qPrintable(QStringLiteral("%1 leaked to the child").arg(k)));
        }
    }

    // An unusable URL must not spawn anything at all.
    void anInvalidUrlIsRefused()
    {
        QVERIFY(!lightning::urls::openExternally(QUrl()));
        QVERIFY(!lightning::urls::openExternally(QUrl(QStringLiteral(""))));
    }
};

QTEST_GUILESS_MAIN(UrlLauncherTest)
#include "UrlLauncherTest.moc"
