#include "app/SettingsManager.h"
#include "models/EmojiCatalog.h"

#include <QCoreApplication>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

class EmojiCatalogTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<QTemporaryDir> m_config;

    QStringList visibleEmoji(EmojiCatalog &catalog) const
    {
        QStringList result;
        for (int row = 0; row < catalog.rowCount(); ++row)
            result.append(catalog.data(catalog.index(row), EmojiCatalog::EmojiRole).toString());
        return result;
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_config = std::make_unique<QTemporaryDir>();
        QVERIFY(m_config->isValid());
        qputenv("XDG_CONFIG_HOME", m_config->path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("LightningEmojiTest"));
        QCoreApplication::setApplicationName(QStringLiteral("catalogue"));
    }

    // Category switching swaps a precomputed bucket: constant time, never a
    // full-catalogue rescan per click. The bound is generous to avoid flakes:
    // 200 switches must finish well inside a second, each landing on a
    // populated, consistent bucket.
    void categorySwitchingIsBucketSwapFast()
    {
        EmojiCatalog catalog(nullptr);
        const QStringList categories = catalog.categories();
        QVERIFY(categories.size() >= 5);
        QElapsedTimer timer;
        timer.start();
        int switches = 0;
        for (int loop = 0; loop < 25 && switches < 200; ++loop) {
            for (const QString &category : categories) {
                if (category == QLatin1String("Recently Used"))
                    continue;
                catalog.setCategory(category);
                QVERIFY(catalog.rowCount() > 0);
                const QString firstCategory =
                    catalog.data(catalog.index(0, 0),
                                 EmojiCatalog::CategoryRole).toString();
                QCOMPARE(firstCategory, category);
                ++switches;
            }
        }
        const qint64 elapsed = timer.elapsed();
        QVERIFY(switches >= 150);
        QVERIFY2(elapsed < 1000,
                 qPrintable(QStringLiteral("%1 category switches took %2 ms")
                                .arg(switches).arg(elapsed)));
    }


    void completeAndUnique()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        QVERIFY2(catalog.catalogueCount() > 3500, "catalogue must include validated tone variants");
        QCOMPARE(catalog.dataVersion(), QStringLiteral("Unicode Emoji 17.0"));
        const QStringList known = {QStringLiteral("😀"), QStringLiteral("❤️"),
            QStringLiteral("👍"), QStringLiteral("👍🏽"), QStringLiteral("👨‍💻"),
            QStringLiteral("🇱🇹"), QStringLiteral("👨‍👩‍👧")};
        for (const QString &emoji : known)
            QVERIFY2(catalog.contains(emoji), qPrintable(emoji));

        QSet<QString> unique;
        const QStringList categories = catalog.categories();
        QCOMPARE(categories.size(), 10);
        for (const QString &category : categories.mid(1)) {
            catalog.setCategory(category);
            QVERIFY2(catalog.rowCount() > 0, qPrintable(category));
            for (const QString &emoji : visibleEmoji(catalog)) {
                QVERIFY(!unique.contains(emoji));
                unique.insert(emoji);
            }
        }
    }

    void search_data()
    {
        QTest::addColumn<QString>("query");
        QTest::addColumn<QString>("expected");
        QTest::newRow("smile") << "smile" << "😄";
        QTest::newRow("heart") << "heart" << "❤️";
        QTest::newRow("thumbs") << "THUMBS UP" << "👍";
        QTest::newRow("developer") << "developer" << "👨‍💻";
        QTest::newRow("lithuania") << "flag lithuania" << "🇱🇹";
        QTest::newRow("cat") << "cat" << "🐈";
        QTest::newRow("alias") << "satisfied" << "😆";
    }

    void search()
    {
        QFETCH(QString, query);
        QFETCH(QString, expected);
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        catalog.setSearchText(query);
        QVERIFY2(visibleEmoji(catalog).contains(expected), qPrintable(query));
        QVERIFY(catalog.rowCount() <= 512);
    }

    void noResult()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        catalog.setSearchText(QStringLiteral("no-such-emoji-query-4b11"));
        QCOMPARE(catalog.rowCount(), 0);
    }

    void tones()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        const QVariantList variants = catalog.variantsFor(QStringLiteral("👍"));
        QCOMPARE(variants.size(), 6);
        QCOMPARE(variants.first().toMap().value(QStringLiteral("emoji")).toString(),
                 QStringLiteral("👍"));
        QCOMPARE(variants[3].toMap().value(QStringLiteral("emoji")).toString(),
                 QStringLiteral("👍🏽"));
        QCOMPARE(catalog.variantsFor(QStringLiteral("😀")).size(), 1);
        catalog.setPreferredTone(QStringLiteral("medium"));
        QCOMPARE(catalog.preferredTone(), QStringLiteral("medium"));
    }

    // Big-emoji detection: one user-perceived sequence counts once, any
    // non-whitespace text disables it, and the count saturates at 4.
    void emojiOnlySequenceCount_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("expected");
        QTest::newRow("one") << QStringLiteral("😀") << 1;
        QTest::newRow("two") << QStringLiteral("😀😀") << 2;
        QTest::newRow("three") << QStringLiteral("😀😀😀") << 3;
        QTest::newRow("four") << QStringLiteral("😀😀😀😀") << 4;
        QTest::newRow("five-saturates") << QStringLiteral("😀😀😀😀😀") << 4;
        QTest::newRow("zwj-family") << QStringLiteral("👨‍👩‍👧‍👦") << 1;
        QTest::newRow("zwj-with-vs16-inside") << QStringLiteral("❤️‍🔥") << 1;
        QTest::newRow("skin-tone") << QStringLiteral("👍🏽") << 1;
        QTest::newRow("flag") << QStringLiteral("🇱🇹") << 1;
        QTest::newRow("keycap") << QStringLiteral("1️⃣") << 1;
        QTest::newRow("hash-keycap") << QStringLiteral("#️⃣") << 1;
        QTest::newRow("vs16-heart") << QStringLiteral("❤️") << 1;
        QTest::newRow("bare-heart-no-vs16") << QStringLiteral("❤") << 1;
        QTest::newRow("profession") << QStringLiteral("👨‍💻") << 1;
        QTest::newRow("three-mixed-kinds")
            << QStringLiteral("👨‍👩‍👧‍👦👍🏽🇱🇹") << 3;
        QTest::newRow("whitespace-around") << QStringLiteral("  😀 ") << 1;
        QTest::newRow("whitespace-between") << QStringLiteral("😀 😀") << 2;
        QTest::newRow("newline-between") << QStringLiteral("😀\n😀") << 2;
        QTest::newRow("text-then-emoji") << QStringLiteral("text 😀") << 0;
        QTest::newRow("emoji-then-text") << QStringLiteral("😀 ok") << 0;
        QTest::newRow("emoji-glued-to-digit") << QStringLiteral("😀4") << 0;
        QTest::newRow("plain-text") << QStringLiteral("hello") << 0;
        QTest::newRow("bare-digit") << QStringLiteral("1") << 0;
        QTest::newRow("bare-hash") << QStringLiteral("#") << 0;
        QTest::newRow("ascii-emoticon") << QStringLiteral(":)") << 0;
        QTest::newRow("empty") << QString() << 0;
        QTest::newRow("only-whitespace") << QStringLiteral("  \n ") << 0;
    }

    void emojiOnlySequenceCount()
    {
        QFETCH(QString, text);
        QFETCH(int, expected);
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        QCOMPARE(catalog.emojiOnlySequenceCount(text), expected);
    }

    // ── Colon-shortcode completion (":thumbs" -> 👍) ─────────────────────
    //
    // On the pre-feature code neither method exists and this file fails to
    // compile at all, which is the sharpest possible regression signal for a
    // method that a future edit could otherwise delete unnoticed.
    void completionsRankThumbsUpFirst()
    {
        SettingsManager settings;
        // Earlier cases persist a skin tone; these expect the plain glyph.
        settings.setPreferredEmojiTone(QString());
        EmojiCatalog catalog(&settings);
        const QVariantList rows = catalog.completionsForPrefix(
            QStringLiteral("thumbs"), 12);
        QVERIFY2(rows.size() >= 2, "expected both thumbs up and thumbs down");
        const QVariantMap first = rows.first().toMap();
        QCOMPARE(first.value(QStringLiteral("kind")).toString(),
                 QStringLiteral("unicode"));
        QCOMPARE(first.value(QStringLiteral("emoji")).toString(),
                 QStringLiteral("👍"));
        QVERIFY2(rows.size() <= 12, "must respect the requested limit");
    }

    // Gemoji-style aliases already ship in the TSV's own column (no separate
    // alias table needed): "+1"/"thumbsup" -> 👍, "heart" -> ❤️, "joy" -> 😂,
    // "fire" -> 🔥, matching what Slack/Discord/GitHub users expect.
    void aliasesFromTheCatalogueResolveExactly_data()
    {
        QTest::addColumn<QString>("code");
        QTest::addColumn<QString>("expected");
        QTest::newRow("+1") << QStringLiteral("+1") << QStringLiteral("👍");
        QTest::newRow("thumbsup") << QStringLiteral("thumbsup") << QStringLiteral("👍");
        QTest::newRow("-1") << QStringLiteral("-1") << QStringLiteral("👎");
        QTest::newRow("heart") << QStringLiteral("heart") << QStringLiteral("❤️");
        QTest::newRow("joy") << QStringLiteral("joy") << QStringLiteral("😂");
        QTest::newRow("fire") << QStringLiteral("fire") << QStringLiteral("🔥");
        QTest::newRow("case-insensitive") << QStringLiteral("THUMBSUP") << QStringLiteral("👍");
    }

    void aliasesFromTheCatalogueResolveExactly()
    {
        QFETCH(QString, code);
        QFETCH(QString, expected);
        SettingsManager settings;
        // Earlier cases persist a skin tone; these expect the plain glyph.
        settings.setPreferredEmojiTone(QString());
        EmojiCatalog catalog(&settings);
        QCOMPARE(catalog.emojiForShortcode(code), expected);
    }

    void unknownShortcodeResolvesToNothing()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        QVERIFY(catalog.emojiForShortcode(QStringLiteral("not-a-real-emoji")).isEmpty());
        QVERIFY(catalog.emojiForShortcode(QString()).isEmpty());
    }

    // Preferred tone applies to both the exact-alias resolver and the
    // fuzzy-search results, so a shortcode and a search result never
    // disagree about which variant the user gets.
    void toneAppliesToShortcodeAndSearchAlike()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        catalog.setPreferredTone(QStringLiteral("medium"));
        QCOMPARE(catalog.emojiForShortcode(QStringLiteral("thumbsup")),
                 QStringLiteral("👍🏽"));
        const QVariantList rows =
            catalog.completionsForPrefix(QStringLiteral("thumbs"), 12);
        QCOMPARE(rows.first().toMap().value(QStringLiteral("emoji")).toString(),
                 QStringLiteral("👍🏽"));
    }

    // Recently used emoji are boosted within their own ranking tier, but a
    // tier match still beats a non-matching recent, and non-recent ties keep
    // catalogue order.
    void recentlyUsedIsBoostedWithinItsTier()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        QVariantList rows = catalog.completionsForPrefix(QStringLiteral("cat"), 12);
        QVERIFY2(rows.size() >= 2, "expected multiple name/alias-prefix cat emoji");
        // Unboosted: catalogue order among the tier-0 "cat" matches, 😹
        // ("cat with tears of joy") sorting ahead of 🐈 ("cat").
        QCOMPARE(rows.first().toMap().value(QStringLiteral("emoji")).toString(),
                 QStringLiteral("😹"));
        int catIndex = -1;
        for (int i = 0; i < rows.size(); ++i) {
            if (rows.at(i).toMap().value(QStringLiteral("emoji")).toString()
                == QStringLiteral("🐈")) { catIndex = i; break; }
        }
        QVERIFY2(catIndex > 0, "🐈 must not already lead with nothing recorded");

        catalog.recordUse(QStringLiteral("🐈"));
        rows = catalog.completionsForPrefix(QStringLiteral("cat"), 12);
        QCOMPARE(rows.first().toMap().value(QStringLiteral("emoji")).toString(),
                 QStringLiteral("🐈"));
        // The non-recent tie (😹 vs the others) is unaffected by the boost.
        QCOMPARE(rows.at(1).toMap().value(QStringLiteral("emoji")).toString(),
                 QStringLiteral("😹"));
    }

    void completionsForPrefixIgnoresEmptyAndBlank()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        QVERIFY(catalog.completionsForPrefix(QString(), 12).isEmpty());
        QVERIFY(catalog.completionsForPrefix(QStringLiteral("   "), 12).isEmpty());
        QVERIFY(catalog.completionsForPrefix(
            QStringLiteral("no-such-emoji-query-4b11"), 12).isEmpty());
    }

    void recentPersistenceAndBound()
    {
        SettingsManager settings;
        EmojiCatalog catalog(&settings);
        // Other cases record uses into the same persisted list.
        catalog.clearRecent();
        catalog.recordUse(QStringLiteral("😀"));
        catalog.recordUse(QStringLiteral("❤️"));
        catalog.recordUse(QStringLiteral("😀"));
        settings.recordRecentEmoji(QStringLiteral("not-an-emoji"));
        catalog.setCategory(QStringLiteral("Recently Used"));
        QCOMPARE(visibleEmoji(catalog), QStringList({QStringLiteral("😀"), QStringLiteral("❤️")}));

        const QStringList flags = {"🇱🇹","🇱🇻","🇪🇪","🇫🇮","🇸🇪","🇳🇴","🇩🇰","🇮🇸",
            "🇩🇪","🇫🇷","🇮🇹","🇪🇸","🇵🇹","🇮🇪","🇬🇧","🇵🇱","🇺🇦","🇨🇿",
            "🇸🇰","🇸🇮","🇭🇷","🇬🇷","🇨🇾","🇲🇹","🇦🇹","🇧🇪","🇳🇱","🇱🇺",
            "🇷🇴","🇧🇬","🇭🇺","🇨🇦","🇺🇸"};
        for (const QString &emoji : flags) catalog.recordUse(emoji);
        QVERIFY(settings.recentEmoji().size() <= 32);
        SettingsManager roundTrip;
        QCOMPARE(roundTrip.recentEmoji(), settings.recentEmoji());
        catalog.clearRecent();
        QCOMPARE(settings.recentEmoji().size(), 0);
    }
};

QTEST_MAIN(EmojiCatalogTest)
#include "EmojiCatalogTest.moc"
