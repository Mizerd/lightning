// B066: a complete shortcode or name outranks prefix matches and recents, so
// ":heart" + Enter inserts the red heart and not heart_eyes.

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
        // heart_eyes (😍) precedes the red heart in the catalogue, so only an
        // exact tier puts the red heart first.
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
        catalog.recordUse(QStringLiteral("\U0001F60D")); // heart_eyes
        const QVariantList rows =
            catalog.completionsForPrefix(QStringLiteral("heart"), 12);
        QVERIFY(rows.size() >= 2);
        QCOMPARE(emojiAt(rows, 0), QStringLiteral("❤️"));
        // The recent one is still the best of the rest.
        QCOMPARE(emojiAt(rows, 1), QStringLiteral("\U0001F60D"));
    }

    void aPartialQueryStillRanksByPrefixAndRecents()
    {
        SettingsManager settings;
        settings.setPreferredEmojiTone(QString());
        EmojiCatalog catalog(&settings);
        // "hear" is nobody's complete alias: catalogue order holds.
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
        // "party" is a name prefix of 🥳 and 🎉 and a complete alias of
        // neither, so the exact tier is empty and only recents can reorder.
        QVariantList rows =
            catalog.completionsForPrefix(QStringLiteral("party"), 12);
        QVERIFY(rows.size() >= 2);
        QCOMPARE(emojiAt(rows, 0), QStringLiteral("\U0001F973")); // partying face
        QCOMPARE(emojiAt(rows, 1), QStringLiteral("\U0001F389")); // party popper
        catalog.recordUse(QStringLiteral("\U0001F389"));
        rows = catalog.completionsForPrefix(QStringLiteral("party"), 12);
        QCOMPARE(emojiAt(rows, 0), QStringLiteral("\U0001F389"));
        QCOMPARE(emojiAt(rows, 1), QStringLiteral("\U0001F973"));
    }
};

QTEST_MAIN(EmojiCatalogExactMatchTest)
#include "EmojiCatalogExactMatchTest.moc"
