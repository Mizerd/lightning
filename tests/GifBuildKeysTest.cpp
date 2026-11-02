
#include "gif/GifBuildKeys.h"

#include <QtTest/QtTest>

using gif::buildKeyFor;
using gif::resolveProviderKey;

class GifBuildKeysTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void noKeysMeansMissing()
    {
        QCOMPARE(resolveProviderKey(QString(), QString()), QString());
        QVERIFY(resolveProviderKey(QString(), QString()).isEmpty());
    }

    void compiledKeyOnly()
    {
        QCOMPARE(resolveProviderKey(QString(), QStringLiteral("BUILDG")),
                 QStringLiteral("BUILDG"));
        QCOMPARE(resolveProviderKey(QString(), QStringLiteral("BUILDK")),
                 QStringLiteral("BUILDK"));
    }

    void runtimeOverridesCompiled()
    {
        QCOMPARE(resolveProviderKey(QStringLiteral("ENVKEY"),
                                    QStringLiteral("BUILDKEY")),
                 QStringLiteral("ENVKEY"));
    }

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

    void trimsWhitespace()
    {
        QCOMPARE(resolveProviderKey(QStringLiteral("  ENVKEY \n"), QString()),
                 QStringLiteral("ENVKEY"));
        QCOMPARE(resolveProviderKey(QString(), QStringLiteral("  BUILDKEY  ")),
                 QStringLiteral("BUILDKEY"));
    }

    void buildKeyForKnownIds()
    {
        QVERIFY(buildKeyFor(QStringLiteral("giphy")).isEmpty());
        QVERIFY(buildKeyFor(QStringLiteral("klipy")).isEmpty());
        QVERIFY(buildKeyFor(QStringLiteral("unknown")).isEmpty());
    }
};

QTEST_MAIN(GifBuildKeysTest)
#include "GifBuildKeysTest.moc"
