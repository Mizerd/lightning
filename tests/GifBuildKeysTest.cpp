// Provider-key resolution: runtime override -> compiled release key -> missing.
// Uses only synthetic values; never a real provider key. buildKeyFor() is
// exercised for both providers (empty in this keyless test build), and the
// precedence/trimming policy is verified directly against resolveProviderKey().

#include "gif/GifBuildKeys.h"

#include <QtTest/QtTest>

using gif::buildKeyFor;
using gif::resolveProviderKey;

class GifBuildKeysTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    // 1. No runtime key and no compiled key -> missing.
    void noKeysMeansMissing()
    {
        QCOMPARE(resolveProviderKey(QString(), QString()), QString());
        QVERIFY(resolveProviderKey(QString(), QString()).isEmpty());
    }

    // 2/3/4. Compiled key only -> that key is used (provider configured).
    void compiledKeyOnly()
    {
        QCOMPARE(resolveProviderKey(QString(), QStringLiteral("BUILDG")),
                 QStringLiteral("BUILDG"));
        QCOMPARE(resolveProviderKey(QString(), QStringLiteral("BUILDK")),
                 QStringLiteral("BUILDK"));
    }

    // 5. Runtime override takes precedence over the compiled value.
    void runtimeOverridesCompiled()
    {
        QCOMPARE(resolveProviderKey(QStringLiteral("ENVKEY"),
                                    QStringLiteral("BUILDKEY")),
                 QStringLiteral("ENVKEY"));
    }

    // 6. Empty / whitespace override does not hide a valid compiled fallback.
    void emptyOverrideKeepsCompiled()
    {
        QCOMPARE(resolveProviderKey(QString(), QStringLiteral("BUILDKEY")),
                 QStringLiteral("BUILDKEY"));
        QCOMPARE(resolveProviderKey(QStringLiteral("   "),
                                    QStringLiteral("BUILDKEY")),
                 QStringLiteral("BUILDKEY"));
        QCOMPARE(resolveProviderKey(QStringLiteral("\t\n"),
                                    QStringLiteral("BUILDKEY")),
                 QStringLiteral("BUILDKEY"));
    }

    // Surrounding whitespace is trimmed from whichever source wins.
    void trimsWhitespace()
    {
        QCOMPARE(resolveProviderKey(QStringLiteral("  ENVKEY \n"), QString()),
                 QStringLiteral("ENVKEY"));
        QCOMPARE(resolveProviderKey(QString(), QStringLiteral("  BUILDKEY  ")),
                 QStringLiteral("BUILDKEY"));
    }

    // buildKeyFor is defined for both providers and empty for anything else. In
    // this keyless developer build both compiled keys are empty, which is the
    // documented "no embedded key" state; an unknown id is always empty.
    void buildKeyForKnownIds()
    {
        QVERIFY(buildKeyFor(QStringLiteral("giphy")).isEmpty());
        QVERIFY(buildKeyFor(QStringLiteral("klipy")).isEmpty());
        QVERIFY(buildKeyFor(QStringLiteral("unknown")).isEmpty());
    }
};

QTEST_MAIN(GifBuildKeysTest)
#include "GifBuildKeysTest.moc"
