
#include "app/SettingsManager.h"
#include "models/EmojiCatalog.h"

#include <QCoreApplication>
#include <QTemporaryDir>
#include <QtTest>

class EmojiCatalogExactMatchTest : public QObject
{
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> m_config;

    static QString emojiAt(const QVariantList &rows, int i)
    {
        return rows.at(i).toMap().value(QStringLiteral("emoji")).toString();
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_config = std::make_unique<QTemporaryDir>();
        QVERIFY(m_config->isValid());
        qputenv("XDG_CONFIG_HOME", m_config->path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("LightningEmojiTest"));
        QCoreApplication::setApplicationName(QStringLiteral("exactmatch"));
    }

    void anExactAliasLeadsItsPrefixMatches()
    {
        SettingsManager settings;
        settings.setPreferredEmojiTone(QString());
        EmojiCatalog catalog(&settings);
        const QVariantList rows =
            catalog.completionsForPrefix(QStringLiteral("heart"), 12);
        QVERIFY(rows.size() >= 2);
        QCOMPARE(emojiAt(rows, 0), QStringLiteral("❤️"));
    }

    void anExactMatchBeatsARecentlyUsedPrefixMatch()
    {
        SettingsManager settings;
        settings.setPreferredEmojiTone(QString());
        EmojiCatalog catalog(&settings);
        catalog.recordUse(QStringLiteral("\U0001F60D"));
        const QVariantList rows =
            catalog.completionsForPrefix(QStringLiteral("heart"), 12);
        QVERIFY(rows.size() >= 2);
        QCOMPARE(emojiAt(rows, 0), QStringLiteral("❤️"));
        QCOMPARE(emojiAt(rows, 1), QStringLiteral("\U0001F60D"));
    }

    void aPartialQueryStillRanksByPrefixAndRecents()
    {
        SettingsManager settings;
        settings.setPreferredEmojiTone(QString());
        EmojiCatalog catalog(&settings);
        const QVariantList plain =
            catalog.completionsForPrefix(QStringLiteral("hear"), 12);
        QVERIFY(!plain.isEmpty());
        QCOMPARE(emojiAt(plain, 0), QStringLiteral("\U0001F60D"));
    }
    void recentsStillBoostWithinATierWhenNothingIsExact()
    {
        SettingsManager settings;
        settings.setPreferredEmojiTone(QString());
        EmojiCatalog catalog(&settings);
        QVariantList rows =
            catalog.completionsForPrefix(QStringLiteral("party"), 12);
        QVERIFY(rows.size() >= 2);
        QCOMPARE(emojiAt(rows, 0), QStringLiteral("\U0001F973"));
        QCOMPARE(emojiAt(rows, 1), QStringLiteral("\U0001F389"));
        catalog.recordUse(QStringLiteral("\U0001F389"));
        rows = catalog.completionsForPrefix(QStringLiteral("party"), 12);
        QCOMPARE(emojiAt(rows, 0), QStringLiteral("\U0001F389"));
        QCOMPARE(emojiAt(rows, 1), QStringLiteral("\U0001F973"));
    }
};

QTEST_MAIN(EmojiCatalogExactMatchTest)
#include "EmojiCatalogExactMatchTest.moc"
