// v0.6.5: focused coverage for EmojiCatalog::recentEmoji — the small
// additive Q_PROPERTY exposing the persisted MRU recent-emoji list directly
// (for a future quick-react strip) alongside the existing filtered/paged
// GridView model. Complements EmojiCatalogTest.cpp's
// recentPersistenceAndBound, which already covers SettingsManager's own
// storage; this suite is scoped to the new accessor's own contract: order,
// filtering of invalid entries, and the change signal.

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

    // Each case starts from a clean store — QSettings is app-global, so
    // recents recorded by one case would otherwise leak into the next
    // (which is exactly how the filter case first failed at integration).
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

        // Re-using an existing entry moves it back to the front rather than
        // duplicating it — same MRU contract as the GridView bucket.
        catalog.recordUse(QStringLiteral("😀"));
        QCOMPARE(catalog.recentEmoji(),
                 QStringList({ QStringLiteral("😀"), QStringLiteral("❤️") }));
    }

    // A corrupted or legacy settings entry (not in the current catalogue —
    // e.g. from an older Unicode revision, or plain garbage) must never
    // reach a consumer of this property, exactly like the GridView bucket's
    // own indexOf() guard in EmojiCatalog::rebuild().
    void filtersEntriesNotInTheCatalogue()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        settings.recordRecentEmoji(QStringLiteral("not-an-emoji"));
        catalog.recordUse(QStringLiteral("😀"));
        QCOMPARE(catalog.recentEmoji(), QStringList({ QStringLiteral("😀") }));
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
