// Colon-shortcode completion for standard Unicode emoji (":thumbs" -> 👍),
// merged into MessageComposer's existing MSC2545 custom-pack popup. Exercises
// MessageComposer's own merge/guard/accept/auto-convert logic against FAKE
// search/resolver callbacks (EmojiCatalog's real ranking is EmojiCatalogTest's
// job) plus a recording MatrixClient for the send-time pass. On the
// pre-feature code this file fails to compile outright: none of
// setUnicodeEmojiSearch, setUnicodeShortcodeResolver,
// acceptUnicodeEmojiCompletionAt or maybeAutoConvertShortcodeBeforeCursor
// existed.

#include "matrix/MatrixClient.h"
#include "models/MessageComposer.h"

#include <QtTest/QtTest>

namespace {

const QString kRoom = QStringLiteral("!room:example.org");
// U+1F44D THUMBS UP SIGN, a surrogate pair: QString::length() == 2.
const QString kThumbsUp = QStringLiteral("👍");

class RecordingClient : public MatrixClient
{
    Q_OBJECT
public:
    explicit RecordingClient(QObject *parent = nullptr) : MatrixClient(parent) {}

    QStringList sentBodies;
    QList<QVariantMap> sentSpecs;

    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return {}; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &userId) const override
    { return userId; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    void sendTextMessage(const QString &, const QString &body) override
    { sentBodies.append(body); }
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void sendTextMessage(const QString &, const QString &body,
                         const QStringList &, const QVariantMap &spec) override
    { sentBodies.append(body); sentSpecs.append(spec); }
    void sendReply(const QString &, const QString &, const QString &body,
                   const QStringList &, const QVariantMap &) override
    { sentBodies.append(body); }
    void sendThreadReplyTo(const QString &, const QString &, const QString &,
                           const QString &body, const QStringList &,
                           const QVariantMap &) override
    { sentBodies.append(body); }
    void editMessage(const QString &, const QString &, const QString &body,
                     const QStringList &, const QVariantMap &) override
    { sentBodies.append(body); }
};

} // namespace

class EmojiShortcodeCompletionTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void init()
    {
        m_client = new RecordingClient(this);
        m_composer = new MessageComposer(this);
        m_composer->setClient(m_client);
        m_composer->setRoomId(kRoom);

        // A fake custom pack: only "blob_wave" resolves, mirroring one
        // installed MSC2545 emoticon.
        m_composer->setEmoticonSearch(
            [](const QString &prefix, int limit) {
                QVariantList out;
                if (QStringLiteral("blob_wave").startsWith(prefix, Qt::CaseInsensitive)) {
                    out.append(QVariantMap{
                        { QStringLiteral("shortcode"), QStringLiteral("blob_wave") },
                        { QStringLiteral("url"), QStringLiteral("mxc://x/1") },
                        { QStringLiteral("packName"), QStringLiteral("Test Pack") },
                    });
                }
                return out.size() > limit ? out.mid(0, limit) : out;
            });
        m_composer->setEmoticonResolver([](const QString &shortcode) {
            return shortcode == QLatin1String("blob_wave")
                ? QStringLiteral("mxc://x/1") : QString();
        });

        // A fake Unicode catalogue: "thumbsup"/"+1" -> 👍, nothing else,
        // standing in for EmojiCatalog's real (separately tested) ranking.
        m_composer->setUnicodeEmojiSearch(
            [](const QString &prefix, int limit) {
                QVariantList out;
                if (QStringLiteral("thumbsup").startsWith(prefix, Qt::CaseInsensitive)) {
                    out.append(QVariantMap{
                        { QStringLiteral("kind"), QStringLiteral("unicode") },
                        { QStringLiteral("emoji"), kThumbsUp },
                        { QStringLiteral("name"), QStringLiteral("thumbs up") },
                        { QStringLiteral("shortcode"), QStringLiteral("thumbsup") },
                    });
                }
                return out.size() > limit ? out.mid(0, limit) : out;
            });
        m_composer->setUnicodeShortcodeResolver([](const QString &code) {
            const QString folded = code.toCaseFolded();
            return (folded == QLatin1String("thumbsup") || folded == QLatin1String("+1"))
                ? kThumbsUp : QString();
        });
        m_composer->setUnicodeEmojiUseRecorder(
            [this](const QString &emoji) { m_recordedUses.append(emoji); });
    }
    void cleanup()
    {
        delete m_composer; m_composer = nullptr;
        delete m_client; m_client = nullptr;
        m_recordedUses.clear();
    }

    // ── Merged popup: custom + Unicode, capped, one-vs-two-char gate ─────

    // Custom pack completions must still be found (existing behaviour
    // intact), and merge with Unicode ones in the same list.
    void customEmoticonsStillFoundAlongsideUnicode()
    {
        m_composer->setText(QStringLiteral(":blob"));
        const QVariantList rows = m_composer->emojiCompletionsAt(5);
        QVERIFY2(!rows.isEmpty(), "custom pack completion must still work");
        QCOMPARE(rows.first().toMap().value(QStringLiteral("shortcode")).toString(),
                 QStringLiteral("blob_wave"));
        // Not tagged "unicode": exactly today's shape, untouched.
        QVERIFY(!rows.first().toMap().contains(QStringLiteral("kind")));
    }

    void thumbsPrefixOffersTheUnicodeThumbsUp()
    {
        m_composer->setText(QStringLiteral(":thumbs"));
        const QVariantList rows = m_composer->emojiCompletionsAt(7);
        QVERIFY2(!rows.isEmpty(), "expected the fake catalogue's thumbsup row");
        bool found = false;
        for (const QVariant &row : rows) {
            const QVariantMap m = row.toMap();
            if (m.value(QStringLiteral("kind")).toString() == QLatin1String("unicode")) {
                QCOMPARE(m.value(QStringLiteral("emoji")).toString(), kThumbsUp);
                found = true;
            }
        }
        QVERIFY2(found, "no Unicode row in the merged popup");
    }

    // A Unicode match needs two query characters; ":D" (a one-character
    // query "D") must not turn into an emoji menu that was never asked for.
    // Custom packs are unaffected: they still complete on one character.
    void unicodeNeedsTwoCharactersCustomNeedsOne()
    {
        m_composer->setUnicodeEmojiSearch([](const QString &, int) {
            // Would match anything; proves the composer's own length gate,
            // not the (fake) catalogue's.
            return QVariantList{ QVariantMap{
                { QStringLiteral("kind"), QStringLiteral("unicode") },
                { QStringLiteral("emoji"), kThumbsUp },
                { QStringLiteral("name"), QStringLiteral("thumbs up") },
                { QStringLiteral("shortcode"), QStringLiteral("thumbsup") },
            } };
        });
        m_composer->setText(QStringLiteral(":D"));
        const QVariantList oneChar = m_composer->emojiCompletionsAt(2);
        for (const QVariant &row : oneChar) {
            QVERIFY2(row.toMap().value(QStringLiteral("kind")).toString()
                         != QLatin1String("unicode"),
                     "a one-character query must not offer a Unicode match");
        }

        m_composer->setText(QStringLiteral(":bl"));
        const QVariantList stillCustom = m_composer->emojiCompletionsAt(3);
        QVERIFY2(!stillCustom.isEmpty(), "custom packs must still complete on one+ chars");
    }

    // ":)"  and "12:30" must never open the popup at all — neither an
    // emoticon nor a time is a shortcode. (emojiTokenAt's own word-boundary
    // and character-class rules; re-asserted here because the merge is the
    // new code path that could have bypassed them.)
    void noPopupForEmoticonsOrTimes_data()
    {
        QTest::addColumn<QString>("text");
        QTest::addColumn<int>("cursor");
        QTest::newRow(":)") << QStringLiteral(":)") << 2;
        QTest::newRow("12:30") << QStringLiteral("12:30") << 5;
    }
    void noPopupForEmoticonsOrTimes()
    {
        QFETCH(QString, text);
        QFETCH(int, cursor);
        m_composer->setText(text);
        QVERIFY(m_composer->emojiCompletionsAt(cursor).isEmpty());
    }

    // ── Accepting a Unicode completion: glyph + space, not `:shortcode:` ──

    void acceptingUnicodeInsertsTheGlyphAndRecordsRecents()
    {
        m_composer->setText(QStringLiteral(":thumb"));
        const int pos = m_composer->acceptUnicodeEmojiCompletionAt(6, kThumbsUp);
        const QString expected = kThumbsUp + QLatin1Char(' ');
        QCOMPARE(m_composer->text(), expected);
        QCOMPARE(pos, expected.length());
        QCOMPARE(m_recordedUses, QStringList{ kThumbsUp });
    }

    // ── `:shortcode:` auto-convert while typing (a space just landed) ────

    void aCompleteAliasConvertsOnceASpaceFollows()
    {
        m_composer->setText(QStringLiteral("hi :thumbsup: "));
        const int pos = m_composer->maybeAutoConvertShortcodeBeforeCursor(14);
        const QString expected = QStringLiteral("hi ") + kThumbsUp + QLatin1Char(' ');
        QVERIFY2(pos >= 0, "expected a completed alias to convert");
        QCOMPARE(m_composer->text(), expected);
        QCOMPARE(pos, expected.length());
        QCOMPARE(m_recordedUses, QStringList{ kThumbsUp });
    }

    void aliasVariantsAllConvert_data()
    {
        QTest::addColumn<QString>("alias");
        QTest::newRow("+1") << QStringLiteral("+1");
        QTest::newRow("thumbsup") << QStringLiteral("thumbsup");
        QTest::newRow("uppercase") << QStringLiteral("THUMBSUP");
    }
    void aliasVariantsAllConvert()
    {
        QFETCH(QString, alias);
        const QString text = QLatin1Char(':') + alias + QStringLiteral(": ");
        m_composer->setText(text);
        const int pos = m_composer->maybeAutoConvertShortcodeBeforeCursor(text.length());
        QVERIFY2(pos >= 0, qPrintable(alias));
        QCOMPARE(m_composer->text(), kThumbsUp + QLatin1Char(' '));
    }

    // Without a trailing space nothing converts live — that half is the
    // send-time pass, tested below.
    void noTrailingSpaceDoesNotConvertLive()
    {
        m_composer->setText(QStringLiteral("hi :thumbsup:"));
        QCOMPARE(m_composer->maybeAutoConvertShortcodeBeforeCursor(13), -1);
        QCOMPARE(m_composer->text(), QStringLiteral("hi :thumbsup:"));
    }

    // A custom pack shortcode of the same name is never shadowed by the
    // Unicode table: today's "stays literal until send" behaviour continues.
    void customPackShortcodeIsNeverAutoConverted()
    {
        m_composer->setText(QStringLiteral("wave :blob_wave: "));
        QCOMPARE(m_composer->maybeAutoConvertShortcodeBeforeCursor(17), -1);
        QCOMPARE(m_composer->text(), QStringLiteral("wave :blob_wave: "));
    }

    // Inside a code span or fence a shortcode is literal text, live or at
    // send — the same rule `insideCode` already enforces for the popup.
    void noConversionInsideACodeSpan()
    {
        // "`x :thumbsup: `" — an opening backtick before the shortcode (with
        // its own space-preceded word boundary) leaves an odd tick count at
        // the cursor, so insideCode() is what refuses this, not the
        // word-boundary rule alone.
        const QString text = QStringLiteral("`x :thumbsup: `");
        m_composer->setText(text);
        QCOMPARE(m_composer->maybeAutoConvertShortcodeBeforeCursor(14), -1);
        QCOMPARE(m_composer->text(), text);
    }

    // Turning the setting off disables both halves.
    void settingOffDisablesAutoConvert()
    {
        m_composer->setEmojiAutoConvertEnabled(false);
        QVERIFY(!m_composer->emojiAutoConvertEnabled());
        m_composer->setText(QStringLiteral("hi :thumbsup: "));
        QCOMPARE(m_composer->maybeAutoConvertShortcodeBeforeCursor(14), -1);

        m_composer->setText(QStringLiteral(":thumbsup:"));
        m_composer->send();
        QCOMPARE(m_client->sentBodies, QStringList{ QStringLiteral(":thumbsup:") });
    }

    // ── The "or send" half: a trailing alias with no space still converts ─

    void sendConvertsATrailingAliasWithNoSpace()
    {
        m_composer->setText(QStringLiteral("nice :thumbsup:"));
        m_composer->send();
        QCOMPARE(m_client->sentBodies,
                 QStringList{ QStringLiteral("nice ") + kThumbsUp });
    }

    void sendLeavesACustomShortcodeLiteralForItsOwnResolver()
    {
        m_composer->setText(QStringLiteral("wave :blob_wave:"));
        m_composer->send();
        // Unconverted by the Unicode pass: sendComposed's own resolver
        // (untouched by this feature) is what turns this into an image.
        QCOMPARE(m_client->sentBodies, QStringList{ QStringLiteral("wave :blob_wave:") });
    }

    void sendDoesNotConvertInsideACodeSpan()
    {
        m_composer->setText(QStringLiteral("`:thumbsup:` literal"));
        m_composer->send();
        QCOMPARE(m_client->sentBodies,
                 QStringList{ QStringLiteral("`:thumbsup:` literal") });
    }

    // ── Explicit selection (Matrix v1.19 image packs) ─────────────────────

    QVariantMap row(const QString &key, const QString &url, const QString &body)
    {
        return QVariantMap{ { QStringLiteral("key"), key },
                            { QStringLiteral("shortcode"), key },
                            { QStringLiteral("url"), url },
                            { QStringLiteral("body"), body } };
    }

    void aChosenImageIsSentNotWhicheverPackOwnsTheShortcodeLater()
    {
        // PACK-LTN-023/024, PACK-PRO-032: two packs have `:blob_wave:`; the
        // user picked the second. The send-time resolver (standing in for
        // "today's packs") would say mxc://x/1 - or nothing when ambiguous.
        m_composer->setText(QStringLiteral("hi :blob"));
        const int pos = m_composer->acceptEmoticonCompletionAt(
            8, row(QStringLiteral("blob_wave"), QStringLiteral("mxc://x/2"),
                   QStringLiteral("A waving blob")));
        QCOMPARE(pos, 15);  // after "hi :blob_wave: "
        QCOMPARE(m_composer->text(), QStringLiteral("hi :blob_wave: "));
        QCOMPARE(m_composer->selectedEmoticons().size(), 1);
        m_composer->send();
        QCOMPARE(m_client->sentSpecs.size(), 1);
        const QVariantMap chosen = m_client->sentSpecs.at(0)
            .value(QStringLiteral("emoticons")).toMap()
            .value(QStringLiteral(":blob_wave:")).toMap();
        QCOMPARE(chosen.value(QStringLiteral("url")).toString(), QStringLiteral("mxc://x/2"));
        QCOMPARE(chosen.value(QStringLiteral("body")).toString(),
                 QStringLiteral("A waving blob"));
        // A send retires the selection with the text.
        QVERIFY(m_composer->selectedEmoticons().isEmpty());
    }

    void editingInsideAChosenTokenMakesItLiteralAndTypingAroundItKeepsIt()
    {
        m_composer->setText(QStringLiteral(":blob"));
        m_composer->acceptEmoticonCompletionAt(
            5, row(QStringLiteral("blob_wave"), QStringLiteral("mxc://x/2"), QString()));
        // Typing before and after (one edit at a time, as an editor sends
        // them) shifts the range; the image survives.
        m_composer->setText(QStringLiteral("so :blob_wave: "));
        m_composer->setText(QStringLiteral("so :blob_wave: yes"));
        QCOMPARE(m_composer->selectedEmoticons().size(), 1);
        QCOMPARE(m_composer->selectedEmoticons().at(0).toMap()
                     .value(QStringLiteral("start")).toInt(), 3);
        // Editing inside it drops the selection: it is literal text now.
        m_composer->setText(QStringLiteral("so :blob_wavy: yes"));
        QVERIFY(m_composer->selectedEmoticons().isEmpty());
    }

    void aSelectionSurvivesARoomRoundTripInMemoryAndNeverReachesTheThreadComposer()
    {
        // PACK-LTN-025 (in-memory half), PACK-CTX-003: the selection belongs
        // to this room's draft, and the shared send pass used by the thread
        // composer never applies it.
        m_composer->setText(QStringLiteral(":blob"));
        m_composer->acceptEmoticonCompletionAt(
            5, row(QStringLiteral("blob_wave"), QStringLiteral("mxc://x/2"), QString()));
        QString body = QStringLiteral(":blob_wave:");
        QVariantMap spec;
        m_composer->applyOutgoingEmoji(body, spec);
        QCOMPARE(spec.value(QStringLiteral("emoticons")).toMap()
                     .value(QStringLiteral(":blob_wave:")).toString(),
                 QStringLiteral("mxc://x/1"));  // the typed rule, not the choice
        m_composer->setRoomId(QStringLiteral("!other:example.org"));
        QVERIFY(m_composer->selectedEmoticons().isEmpty());
        m_composer->setRoomId(kRoom);
        // The draft store is absent in this fixture, so the text did not come
        // back and neither may a selection without its text.
        QVERIFY(m_composer->selectedEmoticons().isEmpty());
    }

    void aPalettePickBindsTheTextItInserted()
    {
        // PACK-LTN-029 / PACK-UX-005: the emoji palette inserts `:key: ` and
        // binds it; a bind against text that is not there records nothing.
        m_composer->setText(QStringLiteral("ok :blob_wave: "));
        QVERIFY(!m_composer->bindEmoticonAt(
            0, row(QStringLiteral("blob_wave"), QStringLiteral("mxc://x/2"), QString())));
        QVERIFY(m_composer->bindEmoticonAt(
            3, row(QStringLiteral("blob_wave"), QStringLiteral("mxc://x/2"), QStringLiteral("d"))));
        m_composer->send();
        QCOMPARE(m_client->sentSpecs.at(0).value(QStringLiteral("emoticons")).toMap()
                     .value(QStringLiteral(":blob_wave:")).toMap()
                     .value(QStringLiteral("url")).toString(),
                 QStringLiteral("mxc://x/2"));
    }

    void aChosenOrAmbiguousCustomNameIsNeverTurnedIntoAUnicodeGlyph()
    {
        // Live run 2026-10-26: a pack image named `wave` chosen from the popup
        // was sent as U+1F44B, because the Unicode pass asked only the
        // (now unique-only) resolver whether a custom image owned the name.
        m_composer->setEmoticonSearch([](const QString &prefix, int) {
            QVariantList out;
            if (QStringLiteral("thumbsup").startsWith(prefix))
                out.append(QVariantMap{ { QStringLiteral("key"), QStringLiteral("thumbsup") },
                                        { QStringLiteral("shortcode"), QStringLiteral("thumbsup") },
                                        { QStringLiteral("url"), QStringLiteral("mxc://x/9") } });
            return out;
        });
        // Two packs own it: the unique resolver answers nothing.
        m_composer->setEmoticonResolver([](const QString &) { return QString(); });
        m_composer->setText(QStringLiteral("typed :thumbsup:"));
        m_composer->send();
        QCOMPARE(m_client->sentBodies.last(), QStringLiteral("typed :thumbsup:"));

        m_composer->setText(QStringLiteral("chosen :thumbs"));
        m_composer->acceptEmoticonCompletionAt(
            14, row(QStringLiteral("thumbsup"), QStringLiteral("mxc://x/9"), QString()));
        m_composer->send();
        QCOMPARE(m_client->sentBodies.last(), QStringLiteral("chosen :thumbsup:"));
        QCOMPARE(m_client->sentSpecs.last().value(QStringLiteral("emoticons")).toMap()
                     .value(QStringLiteral(":thumbsup:")).toMap()
                     .value(QStringLiteral("url")).toString(),
                 QStringLiteral("mxc://x/9"));
    }

    void aSentChosenImageCountsAsOneUseAndADraftDoesNot()
    {
        // PACK-REC-010/011: the custom recents learn from a send, not from a
        // pick that never went out.
        QList<QVariantMap> uses;
        m_composer->setCustomEmojiUseRecorder(
            [&uses](const QVariantMap &row) { uses.append(row); });
        m_composer->setText(QStringLiteral(":blob"));
        m_composer->acceptEmoticonCompletionAt(
            5, row(QStringLiteral("blob_wave"), QStringLiteral("mxc://x/2"), QString()));
        QVERIFY(uses.isEmpty());
        m_composer->send();
        QCOMPARE(uses.size(), 1);
        QCOMPARE(uses.at(0).value(QStringLiteral("url")).toString(), QStringLiteral("mxc://x/2"));
    }

    void aThreadPopupChoiceIsSentByTheThreadAndSpent()
    {
        // The thread composer completes through emojiCompletionEdit over its
        // own text; its choice must reach the thread send (the typed rule
        // would give mxc://x/1), and only once.
        const QVariantMap edit = m_composer->emojiCompletionEdit(
            QStringLiteral("t :blob"), 7,
            row(QStringLiteral("blob_wave"), QStringLiteral("mxc://x/2"), QStringLiteral("d")));
        QCOMPARE(edit.value(QStringLiteral("replacement")).toString(),
                 QStringLiteral(":blob_wave: "));
        QString body = QStringLiteral("t :blob_wave:");
        QVariantMap spec;
        m_composer->applyOutgoingEmoji(body, spec);
        QCOMPARE(spec.value(QStringLiteral("emoticons")).toMap()
                     .value(QStringLiteral(":blob_wave:")).toMap()
                     .value(QStringLiteral("url")).toString(),
                 QStringLiteral("mxc://x/2"));
        QString again = QStringLiteral("t :blob_wave:");
        QVariantMap spec2;
        m_composer->applyOutgoingEmoji(again, spec2);
        QCOMPARE(spec2.value(QStringLiteral("emoticons")).toMap()
                     .value(QStringLiteral(":blob_wave:")).toString(),
                 QStringLiteral("mxc://x/1"));
        // The main composer's own send never sees the thread's choice.
        QVERIFY(m_composer->selectedEmoticons().isEmpty());
    }

    void anAmbiguousTypedShortcodeStaysLiteral()
    {
        // The resolver answers "" for an ambiguous key (uniqueEmoticonUrl);
        // nothing is substituted rather than picking the first pack.
        m_composer->setEmoticonResolver([](const QString &) { return QString(); });
        m_composer->setText(QStringLiteral("x :blob_wave: y"));
        m_composer->send();
        QCOMPARE(m_client->sentSpecs.size(), 1);
        QVERIFY(!m_client->sentSpecs.at(0).contains(QStringLiteral("emoticons")));
    }

private:
    RecordingClient *m_client = nullptr;
    MessageComposer *m_composer = nullptr;
    QStringList m_recordedUses;
};

QTEST_GUILESS_MAIN(EmojiShortcodeCompletionTest)
#include "EmojiShortcodeCompletionTest.moc"
