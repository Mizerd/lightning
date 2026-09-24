// EmojiCatalog::recentEmoji, the Q_PROPERTY exposing the persisted MRU list
// alongside the GridView model: order, filtering of invalid entries and the
// change signal. EmojiCatalogTest::recentPersistenceAndBound covers
// SettingsManager's storage.

#include "app/SettingsManager.h"
#include "models/EmojiCatalog.h"

#include <QCoreApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

class EmojiCatalogRecentEmojiTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_config;

private Q_SLOTS:
    void initTestCase()
    {
        m_config = std::make_unique<QTemporaryDir>();
        QVERIFY(m_config->isValid());
        qputenv("XDG_CONFIG_HOME", m_config->path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("LightningEmojiRecentTest"));
        QCoreApplication::setApplicationName(QStringLiteral("recent"));
    }

    // Each case starts from a clean store: QSettings is app-global, so recents
    // would leak between cases.
    void init() { QSettings().clear(); }

    void withoutSettingsReturnsEmpty()
    {
        EmojiCatalog catalog(nullptr);
        QVERIFY(catalog.recentEmoji().isEmpty());
    }

    void reflectsSettingsInMruOrder()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        QVERIFY(catalog.recentEmoji().isEmpty());

        catalog.recordUse(QStringLiteral("😀"));
        catalog.recordUse(QStringLiteral("❤️"));
        QCOMPARE(catalog.recentEmoji(),
                 QStringList({ QStringLiteral("❤️"), QStringLiteral("😀") }));

        // Re-using an entry moves it to the front rather than duplicating it,
        // as in the GridView bucket.
        catalog.recordUse(QStringLiteral("😀"));
        QCOMPARE(catalog.recentEmoji(),
                 QStringList({ QStringLiteral("😀"), QStringLiteral("❤️") }));
    }

    // A corrupted or legacy entry (not in the current catalogue) never reaches
    // a consumer, matching the GridView bucket's indexOf() guard in
    // EmojiCatalog::rebuild().
    void filtersEntriesNotInTheCatalogue()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        settings.recordRecentEmoji(QStringLiteral("not-an-emoji"));
        catalog.recordUse(QStringLiteral("😀"));
        QCOMPARE(catalog.recentEmoji(), QStringList({ QStringLiteral("😀") }));
    }

    // A fresh account's picker does not open on an empty "Recently Used"
    // grid; rebuild() has no fallback of its own.
    void afreshCatalogueDoesNotOpenOnAnEmptyRecentlyUsed()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        QVERIFY(catalog.recentEmoji().isEmpty());
        QVERIFY2(catalog.category() != QStringLiteral("Recently Used"),
                 "a catalogue with no recents still starts on Recently Used");
        QCOMPARE(catalog.category(), QStringLiteral("Smileys & Emotion"));
        QVERIFY2(catalog.rowCount() > 0,
                 qPrintable(QStringLiteral(
                     "the picker opens on '%1' and it is EMPTY")
                        .arg(catalog.category())));
        // The tab still exists, first in the rail, and holds the empty state.
        QCOMPARE(catalog.categories().constFirst(),
                 QStringLiteral("Recently Used"));
    }

    // ...and once there are recents, the picker opens there.
    void acatalogueWithRecentsStillStartsOnRecentlyUsed()
    {
        SettingsManager settings;
        settings.recordRecentEmoji(QStringLiteral("😀"));
        EmojiCatalog catalog(&settings);
        QCOMPARE(catalog.recentEmoji(), QStringList({ QStringLiteral("😀") }));
        QCOMPARE(catalog.category(), QStringLiteral("Recently Used"));
        QCOMPARE(catalog.rowCount(), 1);
    }

    // An entry the catalogue cannot resolve is not a recent: rebuild() drops
    // it, so counting it would reopen the empty grid. Both halves apply the
    // same filter.
    void astaleRecentEntryDoesNotCountAsARecent()
    {
        SettingsManager settings;
        settings.recordRecentEmoji(QStringLiteral("not-an-emoji"));
        EmojiCatalog catalog(&settings);
        QVERIFY(catalog.recentEmoji().isEmpty());
        QCOMPARE(catalog.category(), QStringLiteral("Smileys & Emotion"));
        QVERIFY(catalog.rowCount() > 0);
    }

    void emitsChangedSignalOnRecordAndClear()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        QSignalSpy spy(&catalog, &EmojiCatalog::recentEmojiChanged);

        catalog.recordUse(QStringLiteral("😀"));
        QCOMPARE(spy.count(), 1);

        catalog.clearRecent();
        QCOMPARE(spy.count(), 2);
        QVERIFY(catalog.recentEmoji().isEmpty());
    }
};

QTEST_MAIN(EmojiCatalogRecentEmojiTest)
#include "EmojiCatalogRecentEmojiTest.moc"
