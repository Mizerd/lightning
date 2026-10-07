// Chat backgrounds and surface depth.
//
// The wire format is covered in rust/src/backdrop.rs. This is the policy half:
//   * which background a room shows (precedence and the two opt-outs);
//   * that an answer from another session or another read never lands;
//   * that the scrim keeps the timeline's ink readable on EVERY shipped
//     preset, checked by an independent recomputation here, and that the naive
//     alternative (a fixed 50% dim) would not;
//   * that a picked picture is re-encoded with nothing of the original file;
//   * that gradients are sanitised, round-trip, and are graded at their worst
//     stop, and that "Depth" passes every readability check on every preset.

#include "app/CustomThemeStore.h"
#include "app/SettingsManager.h"
#include "backdrop/BackdropMath.h"
#include "backdrop/ChatBackdropController.h"
#include "matrix/MockMatrixClient.h"
#include "matrix/RoomInfo.h"
#include "media/SvgRasterJob.h"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <cmath>
#include <functional>

namespace {

// ---- an independent contrast implementation --------------------------------
// Deliberately NOT BackdropMath's: the floor is verified by a second
// implementation, so a shared bug cannot certify itself.

double channel(int v)
{
    const double c = v / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double lum(const QColor &c)
{
    const QColor r = c.toRgb();
    return 0.2126 * channel(r.red()) + 0.7152 * channel(r.green())
        + 0.0722 * channel(r.blue());
}

double ratio(const QColor &a, const QColor &b)
{
    const double la = lum(a), lb = lum(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QColor over(const QColor &top, double alpha, const QColor &bottom)
{
    const QColor t = top.toRgb(), u = bottom.toRgb();
    auto ch = [alpha](int a, int b) { return int(std::lround(alpha * a + (1 - alpha) * b)); };
    return QColor(ch(t.red(), u.red()), ch(t.green(), u.green()), ch(t.blue(), u.blue()));
}

// ---- the eleven presets, read from AppTheme.qml as CustomThemeTest does ----

QString appTheme()
{
    QFile f(QStringLiteral(APPTHEME_QML_PATH));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

QHash<QString, QHash<QString, QString>> presetPalettes(const QString &qml)
{
    QHash<QString, QString> literals;
    static const QRegularExpression lit(
        QStringLiteral("readonly\\s+property\\s+color\\s+(_\\w+):\\s*"
                       "\"(#[0-9A-Fa-f]{6})\""));
    auto lt = lit.globalMatch(qml);
    while (lt.hasNext()) {
        const auto m = lt.next();
        literals.insert(m.captured(1), m.captured(2).toUpper());
    }
    const QHash<QString, QString> names{
        {QStringLiteral("_light"), QStringLiteral("Lightning Light")},
        {QStringLiteral("_dark"), QStringLiteral("Lightning Dark")},
        {QStringLiteral("_graphite"), QStringLiteral("Graphite")},
        {QStringLiteral("_midnight"), QStringLiteral("Midnight")},
        {QStringLiteral("_nord"), QStringLiteral("Nordic")},
        {QStringLiteral("_purple"), QStringLiteral("Purple Dusk")},
        {QStringLiteral("_warm"), QStringLiteral("Warm")},
        {QStringLiteral("_moss"), QStringLiteral("Moss Light")},
        {QStringLiteral("_indigo"), QStringLiteral("Indigo Night")},
        {QStringLiteral("_teal"), QStringLiteral("Deep Teal")},
        {QStringLiteral("_storm"), QStringLiteral("Storm")}};
    QHash<QString, QHash<QString, QString>> out;
    static const QRegularExpression block(
        QStringLiteral("readonly\\s+property\\s+var\\s+(_\\w+):\\s*\\(\\{"
                       "(.*?)\\n    \\}\\)"),
        QRegularExpression::DotMatchesEverythingOption);
    auto bt = block.globalMatch(qml);
    while (bt.hasNext()) {
        const auto m = bt.next();
        if (!names.contains(m.captured(1)))
            continue;
        QString body = m.captured(2);
        body.remove(QRegularExpression(QStringLiteral("//[^\n]*")));
        QHash<QString, QString> palette;
        static const QRegularExpression entry(
            QStringLiteral("(\\w+)\\s*:\\s*([_\\w]+|\"#[0-9A-Fa-f]{6}\")"));
        auto et = entry.globalMatch(body);
        while (et.hasNext()) {
            const auto e = et.next();
            const QString value = e.captured(2);
            if (value.startsWith(QLatin1Char('"')))
                palette.insert(e.captured(1), value.mid(1, 7).toUpper());
            else if (literals.contains(value))
                palette.insert(e.captured(1), literals.value(value));
        }
        out.insert(names.value(m.captured(1)), palette);
    }
    return out;
}

QString colorLiteral(const QString &qml, const QString &name)
{
    const QRegularExpression re(
        QStringLiteral("readonly\\s+property\\s+color\\s+%1:\\s*"
                       "\"(#[0-9A-Fa-f]{6})\"").arg(name));
    const auto m = re.match(qml);
    return m.hasMatch() ? m.captured(1).toUpper() : QString();
}

// paletteForTheme()'s spellings and fallbacks, as CustomThemeTest resolves
// them, so the audit reads the same object QML hands it.
QVariantMap resolvedPalette(const QHash<QString, QString> &raw,
                            const QString &dangerFill)
{
    QVariantMap p;
    for (auto it = raw.constBegin(); it != raw.constEnd(); ++it)
        p.insert(it.key(), it.value());
    const auto fallback = [&](const char *key, const QString &value) {
        if (!p.contains(QLatin1String(key)) && !value.isEmpty())
            p.insert(QLatin1String(key), value);
    };
    if (raw.contains(QStringLiteral("inputBg")))
        p.insert(QStringLiteral("inputBackground"), raw.value(QStringLiteral("inputBg")));
    p.insert(QStringLiteral("reactionBackground"),
             raw.value(QStringLiteral("reaction"), raw.value(QStringLiteral("cardElevated"))));
    const QString mention = raw.value(QStringLiteral("mention"), dangerFill);
    if (!mention.isEmpty())
        p.insert(QStringLiteral("mentionBadge"), mention);
    p.insert(QStringLiteral("ownBubbleText"), QStringLiteral("#FFFFFF"));
    fallback("rail", raw.value(QStringLiteral("sidebar")));
    fallback("accentText", QStringLiteral("#FFFFFF"));
    fallback("link", raw.value(QStringLiteral("accent")));
    fallback("selectedText", raw.value(QStringLiteral("textPrimary")));
    fallback("otherBubble", raw.value(QStringLiteral("cardElevated")));
    return p;
}

bool isDark(const QString &background)
{
    return lum(QColor::fromString(background)) < 0.18;
}

QImage solid(const QColor &c, int w = 64, int h = 40)
{
    QImage img(w, h, QImage::Format_ARGB32);
    img.fill(c);
    return img;
}

// ---- a client that records what it is asked ---------------------------------

struct Ask {
    QString scope;
    quint64 opId = 0;
};

class FakeBackdropClient : public MockMatrixClient
{
public:
    using MockMatrixClient::MockMatrixClient;

    bool supportsRoomBackgrounds() const override { return supports; }
    void fetchRoomBackground(const QString &roomId, quint64 opId) override
    {
        asks.append({ roomId, opId });
    }
    void setRoomBackground(const QString &roomId, const QString &localPath,
                           const QString &contentJson, quint64 opId) override
    {
        writes.append({ roomId, opId });
        lastPath = localPath;
        lastContent = contentJson;
    }
    QList<RoomInfo> rooms() const override { return roomList; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }

    bool supports = true;
    QList<Ask> asks;
    QList<Ask> writes;
    QString lastPath;
    QString lastContent;
    QList<RoomInfo> roomList;

    quint64 opFor(const QString &scope) const
    {
        for (int i = int(asks.size()) - 1; i >= 0; --i) {
            if (asks.at(i).scope == scope)
                return asks.at(i).opId;
        }
        return 0;
    }
};

RoomInfo room(const QString &id, bool space = false,
              const QStringList &children = {})
{
    RoomInfo r;
    r.id = id;
    r.isSpace = space;
    r.childRoomIds = children;
    r.membership = RoomInfo::Joined;
    return r;
}

// Content as a room carries it: dim/blur/tint are INTEGER percentages
// (canonical JSON has no floats).
QVariantMap shared(const QString &url, int dim = 20)
{
    return QVariantMap{
        { QStringLiteral("version"), 1 },
        { QStringLiteral("url"), url },
        { QStringLiteral("presentation"),
          QVariantMap{ { QStringLiteral("dim"), dim } } },
    };
}

const QString kRoom = QStringLiteral("!room:example.org");
const QString kSpace = QStringLiteral("!space:example.org");
const QString kParent = QStringLiteral("!parent:example.org");

} // namespace

class ChatBackdropTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_configHome;

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(QStringLiteral("chat-backdrop-test"));
    }

    void init()
    {
        QSettings settings;
        settings.clear();
        settings.sync();
    }

    // ---- precedence --------------------------------------------------------

    // personal room > shared room > shared Space (nearest first) > personal
    // default > none. Each step removes the winner and checks the next.
    void precedenceIsPersonalRoomThenRoomThenSpaceThenDefault()
    {
        backdrop::ResolveInput in;
        in.personalRoom = { { QStringLiteral("file"), QStringLiteral("p") } };
        in.sharedRoom = shared(QStringLiteral("mxc://e.org/room"));
        in.sharedSpaces = { qMakePair(kSpace, shared(QStringLiteral("mxc://e.org/near"))),
                            qMakePair(kParent, shared(QStringLiteral("mxc://e.org/far"))) };
        in.personalDefault = { { QStringLiteral("file"), QStringLiteral("d") } };

        QCOMPARE(backdrop::resolve(kRoom, in).source, QStringLiteral("personal-room"));
        in.personalRoom.clear();
        auto r = backdrop::resolve(kRoom, in);
        QCOMPARE(r.source, QStringLiteral("room"));
        QCOMPARE(r.scopeId, kRoom);
        in.sharedRoom.clear();
        r = backdrop::resolve(kRoom, in);
        QCOMPARE(r.source, QStringLiteral("space"));
        QCOMPARE(r.scopeId, kSpace);   // the NEAREST Space, not the root
        in.sharedSpaces[0].second.clear();
        r = backdrop::resolve(kRoom, in);
        QCOMPARE(r.scopeId, kParent);
        in.sharedSpaces.clear();
        QCOMPARE(backdrop::resolve(kRoom, in).source, QStringLiteral("personal-default"));
        in.personalDefault.clear();
        QCOMPARE(backdrop::resolve(kRoom, in).source, QStringLiteral("none"));
    }

    // The opt-outs remove BOTH shared levels and nothing else: the personal
    // default shows instead, and a personal room choice is never affected.
    void theOptOutsRemoveOnlyTheSharedLevels()
    {
        backdrop::ResolveInput in;
        in.sharedRoom = shared(QStringLiteral("mxc://e.org/room"));
        in.sharedSpaces = { qMakePair(kSpace, shared(QStringLiteral("mxc://e.org/space"))) };
        in.personalDefault = { { QStringLiteral("file"), QStringLiteral("d") } };

        in.showShared = false;
        QCOMPARE(backdrop::resolve(kRoom, in).source, QStringLiteral("personal-default"));
        in.showShared = true;
        in.roomHidden = true;
        QCOMPARE(backdrop::resolve(kRoom, in).source, QStringLiteral("personal-default"));
        in.personalDefault.clear();
        QCOMPARE(backdrop::resolve(kRoom, in).source, QStringLiteral("none"));
        in.personalRoom = { { QStringLiteral("file"), QStringLiteral("p") } };
        QCOMPARE(backdrop::resolve(kRoom, in).source, QStringLiteral("personal-room"));
    }

    // A room in two Spaces inherits from the one the user is browsing; then
    // that Space's ancestors, nearest first; a parent cycle terminates.
    void theSpaceChainFollowsTheActiveSpaceAndSurvivesACycle()
    {
        QHash<QString, QStringList> children{
            { QStringLiteral("!a"), { kRoom } },
            { QStringLiteral("!b"), { kRoom } },
            { QStringLiteral("!root"), { QStringLiteral("!b") } },
        };
        const QStringList order{ QStringLiteral("!a"), QStringLiteral("!b"),
                                 QStringLiteral("!root") };
        QCOMPARE(backdrop::spaceChain(kRoom, children, order, QString()),
                 QStringList{ QStringLiteral("!a") });
        QCOMPARE(backdrop::spaceChain(kRoom, children, order, QStringLiteral("!root")),
                 (QStringList{ QStringLiteral("!b"), QStringLiteral("!root") }));
        // A cycle root -> b -> root must not spin.
        children[QStringLiteral("!b")].append(QStringLiteral("!root"));
        const QStringList chain =
            backdrop::spaceChain(kRoom, children, order, QStringLiteral("!b"));
        QVERIFY(chain.size() <= backdrop::kMaxSpaceDepth + 1);
        QCOMPARE(chain.first(), QStringLiteral("!b"));
        QCOMPARE(backdrop::spaceChain(QStringLiteral("!lonely"), children, order, {}),
                 QStringList());
    }

    // ---- the controller: asking, stale answers, opt-outs --------------------

    void aRoomAndItsSpacesAreAskedOnceAndPermissionIsNeverGuessed()
    {
        FakeBackdropClient client;
        client.roomList = { room(kRoom), room(kSpace, true, { kRoom }),
                            room(kParent, true, { kSpace }) };
        ChatBackdropController backdrops;
        backdrops.setClient(&client);
        QVERIFY(backdrops.sharedAvailable());

        QCOMPARE(backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));
        QVERIFY(!backdrops.canSetShared(kRoom));

        backdrops.requestRoom(kRoom);
        QCOMPARE(client.asks.size(), 3);   // room, Space, parent Space
        backdrops.requestRoom(kRoom);
        QCOMPARE(client.asks.size(), 3);   // within the refresh interval

        QSignalSpy revisions(&backdrops, &ChatBackdropController::revisionChanged);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kParent), kParent,
                                             shared(QStringLiteral("mxc://e.org/far")),
                                             false, false);
        QVariantMap b = backdrops.backdropFor(kRoom);
        QCOMPARE(b.value(QStringLiteral("source")).toString(), QStringLiteral("space"));
        QCOMPARE(b.value(QStringLiteral("scopeId")).toString(), kParent);
        QCOMPARE(b.value(QStringLiteral("kind")).toString(), QStringLiteral("mxc"));
        QCOMPARE(b.value(QStringLiteral("mxc")).toString(), QStringLiteral("mxc://e.org/far"));

        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             shared(QStringLiteral("mxc://e.org/room")),
                                             true, false);
        b = backdrops.backdropFor(kRoom);
        QCOMPARE(b.value(QStringLiteral("source")).toString(), QStringLiteral("room"));
        QVERIFY(backdrops.canSetShared(kRoom));
        QVERIFY(!backdrops.canSetShared(kParent));
        QCOMPARE(revisions.count(), 2);
    }

    // An answer this session did not ask for, or for a different room than the
    // op was for, never lands; nor does one that arrives after sign-out.
    void staleAndMismatchedAnswersAreDropped()
    {
        FakeBackdropClient client;
        client.roomList = { room(kRoom) };
        ChatBackdropController backdrops;
        backdrops.setClient(&client);

        Q_EMIT client.roomBackgroundReceived(424242, kRoom,
                                             shared(QStringLiteral("mxc://e.org/x")),
                                             true, false);
        QCOMPARE(backdrops.sharedFor(kRoom), QVariantMap());

        backdrops.requestRoom(kRoom);
        const quint64 op = client.opFor(kRoom);
        Q_EMIT client.roomBackgroundReceived(op, QStringLiteral("!other:example.org"),
                                             shared(QStringLiteral("mxc://e.org/x")),
                                             true, false);
        QCOMPARE(backdrops.sharedFor(QStringLiteral("!other:example.org")), QVariantMap());

        // Sign-out between the ask and the answer.
        Q_EMIT client.loggedOut();
        Q_EMIT client.roomBackgroundReceived(op, kRoom,
                                             shared(QStringLiteral("mxc://e.org/x")),
                                             true, false);
        QCOMPARE(backdrops.sharedFor(kRoom), QVariantMap());
        QVERIFY(!backdrops.canSetShared(kRoom));
    }

    // VM test 2026-10-07: a background another member set while the room was
    // OPEN appeared only after leaving and re-opening it, because the shared
    // state was read on open alone (rate-limited) and nothing listened to
    // sync. A change observed in sync must re-read the open room at once, and
    // a Space's change must reach the rooms that inherit it. Old code: no
    // second ask, the room keeps showing "none".
    void aChangeInSyncRereadsTheOpenRoomAndItsSpaceAtOnce()
    {
        FakeBackdropClient client;
        client.roomList = { room(kRoom), room(kSpace, true, { kRoom }) };
        ChatBackdropController backdrops;
        backdrops.setClient(&client);
        backdrops.requestRoom(kRoom);
        QCOMPARE(client.asks.size(), 2);   // the room and its Space
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             QVariantMap(), false, false);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kSpace), kSpace,
                                             QVariantMap(), false, false);
        QCOMPARE(backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));

        // Set in the open room, well inside the open-time refresh interval.
        Q_EMIT client.roomBackgroundChanged(kRoom);
        QCOMPARE(client.asks.size(), 3);
        QCOMPARE(client.asks.last().scope, kRoom);
        QSignalSpy revisions(&backdrops, &ChatBackdropController::revisionChanged);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             shared(QStringLiteral("mxc://e.org/new")),
                                             false, false);
        QVariantMap b = backdrops.backdropFor(kRoom);
        QCOMPARE(b.value(QStringLiteral("source")).toString(), QStringLiteral("room"));
        QCOMPARE(b.value(QStringLiteral("mxc")).toString(), QStringLiteral("mxc://e.org/new"));
        QCOMPARE(revisions.count(), 1);

        // Cleared again, then the Space gets a picture: the room inherits it.
        Q_EMIT client.roomBackgroundChanged(kRoom);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             QVariantMap(), false, false);
        Q_EMIT client.roomBackgroundChanged(kSpace);
        QCOMPARE(client.asks.last().scope, kSpace);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kSpace), kSpace,
                                             shared(QStringLiteral("mxc://e.org/space")),
                                             false, false);
        b = backdrops.backdropFor(kRoom);
        QCOMPARE(b.value(QStringLiteral("source")).toString(), QStringLiteral("space"));
        QCOMPARE(b.value(QStringLiteral("scopeId")).toString(), kSpace);

        // A room this session never showed costs no request.
        const qsizetype before = client.asks.size();
        Q_EMIT client.roomBackgroundChanged(QStringLiteral("!elsewhere:example.org"));
        QCOMPARE(client.asks.size(), before);

        // A change while a read is in flight reads once more when it lands,
        // since the answer in flight may predate the change.
        Q_EMIT client.roomBackgroundChanged(kRoom);
        QCOMPARE(client.asks.size(), before + 1);
        Q_EMIT client.roomBackgroundChanged(kRoom);
        QCOMPARE(client.asks.size(), before + 1);   // one read per scope
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             QVariantMap(), false, false);
        QCOMPARE(client.asks.size(), before + 2);
        QCOMPARE(client.asks.last().scope, kRoom);

        // Answers belong to the session: after sign-out nothing re-reads.
        Q_EMIT client.loggedOut();
        const qsizetype afterSignOut = client.asks.size();
        Q_EMIT client.roomBackgroundChanged(kRoom);
        QCOMPARE(client.asks.size(), afterSignOut);
    }

    // A non-mxc url can never become a background, even if a backend let one
    // through; a newer schema is reported, not rendered.
    void onlyMxcRendersAndANewerSchemaIsReported()
    {
        FakeBackdropClient client;
        client.roomList = { room(kRoom) };
        ChatBackdropController backdrops;
        backdrops.setClient(&client);
        backdrops.requestRoom(kRoom);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             shared(QStringLiteral("https://evil.example/p.png")),
                                             true, false);
        QCOMPARE(backdrops.backdropFor(kRoom).value(QStringLiteral("kind")).toString(),
                 QStringLiteral("none"));

        backdrops.refreshScope(kRoom);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom, QVariantMap(),
                                             true, /*unsupportedVersion=*/true);
        QVERIFY(backdrops.sharedUnsupported(kRoom));
        QCOMPARE(backdrops.backdropFor(kRoom).value(QStringLiteral("kind")).toString(),
                 QStringLiteral("none"));
    }

    void showSharedAndHideRoomReachTheController()
    {
        SettingsManager settings;
        FakeBackdropClient client;
        client.roomList = { room(kRoom) };
        ChatBackdropController backdrops;
        backdrops.setSettings(&settings);
        backdrops.setClient(&client);
        QVERIFY(backdrops.showShared());   // default ON
        backdrops.requestRoom(kRoom);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             shared(QStringLiteral("mxc://e.org/r")),
                                             true, false);
        QCOMPARE(backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("room"));

        backdrops.setShowShared(false);
        QCOMPARE(backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));
        backdrops.setShowShared(true);
        backdrops.setRoomHidden(kRoom, true);
        QVERIFY(backdrops.roomHidden(kRoom));
        QCOMPARE(backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));
        backdrops.setRoomHidden(kRoom, false);
        QCOMPARE(backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("room"));
    }

    // A presentation-only change re-sends the SAME picture and uploads
    // nothing; clearing sends no url and no path; a write answered for another
    // op does not land.
    void sharedWritesReuseThePictureAndClearSendsNothing()
    {
        FakeBackdropClient client;
        client.roomList = { room(kRoom) };
        ChatBackdropController backdrops;
        backdrops.setClient(&client);
        backdrops.requestRoom(kRoom);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             shared(QStringLiteral("mxc://e.org/r")),
                                             true, false);

        backdrops.setShared(kRoom, QVariantMap{ { QStringLiteral("dim"), 0.6 } });
        QCOMPARE(client.writes.size(), 1);
        QVERIFY(client.lastPath.isEmpty());
        QVERIFY(client.lastContent.contains(QStringLiteral("mxc://e.org/r")));
        QVERIFY(backdrops.busy());
        // Busy: a second write is refused until the first answers.
        backdrops.clearShared(kRoom);
        QCOMPARE(client.writes.size(), 1);

        Q_EMIT client.roomBackgroundSet(client.writes.first().opId + 99, kRoom, true,
                                        QVariantMap(), QString());
        QVERIFY(backdrops.busy());
        Q_EMIT client.roomBackgroundSet(client.writes.first().opId, kRoom, true,
                                        shared(QStringLiteral("mxc://e.org/r"), 60),
                                        QString());
        QVERIFY(!backdrops.busy());

        backdrops.clearShared(kRoom);
        QCOMPARE(client.writes.size(), 2);
        QVERIFY(client.lastPath.isEmpty());
        QVERIFY(!client.lastContent.contains(QStringLiteral("url")));
        Q_EMIT client.roomBackgroundSet(client.writes.last().opId, kRoom, true,
                                        QVariantMap(), QString());
        QCOMPARE(backdrops.sharedFor(kRoom), QVariantMap());
    }

    // The defect behind "The background could not be saved." (2026-10-06):
    // the controller sent dim/blur/tint as 0.2-style floats, and Synapse
    // refuses ANY float in event content (400 M_BAD_JSON "Bad JSON value:
    // float"), so every shared write failed. What is sent must be integers;
    // what is read back is converted to the app's unit values. Fails on the
    // float version: "dim" arrives as 0.45 and the walk finds a fraction.
    void sharedWritesSendIntegerPercentagesNeverFloats()
    {
        FakeBackdropClient client;
        client.roomList = { room(kRoom) };
        ChatBackdropController backdrops;
        backdrops.setClient(&client);
        backdrops.requestRoom(kRoom);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             shared(QStringLiteral("mxc://e.org/r"), 30),
                                             true, false);
        // Read back in the app's units.
        QCOMPARE(backdrops.sharedFor(kRoom).value(QStringLiteral("presentation")).toMap()
                     .value(QStringLiteral("dim")).toDouble(), 0.3);

        backdrops.setShared(kRoom, { { QStringLiteral("dim"), 0.45 },
                                     { QStringLiteral("blur"), 0.2 },
                                     { QStringLiteral("tint"), 0.1 } });
        QCOMPARE(client.writes.size(), 1);
        const QJsonDocument doc = QJsonDocument::fromJson(client.lastContent.toUtf8());
        QVERIFY(doc.isObject());
        QStringList fractions;
        std::function<void(const QJsonValue &, const QString &)> walk =
            [&](const QJsonValue &v, const QString &at) {
                if (v.isDouble() && std::floor(v.toDouble()) != v.toDouble())
                    fractions << at;
                if (v.isObject()) {
                    const QJsonObject o = v.toObject();
                    for (auto it = o.begin(); it != o.end(); ++it)
                        walk(it.value(), at + QLatin1Char('.') + it.key());
                }
                if (v.isArray()) {
                    const QJsonArray a = v.toArray();
                    for (int i = 0; i < a.size(); ++i)
                        walk(a.at(i), at + QStringLiteral("[%1]").arg(i));
                }
            };
        walk(QJsonValue(doc.object()), QStringLiteral("content"));
        QVERIFY2(fractions.isEmpty(),
                 qPrintable(QStringLiteral("floats in event content: %1")
                                .arg(fractions.join(QStringLiteral(", ")))));
        const QJsonObject p = doc.object().value(QStringLiteral("presentation")).toObject();
        QCOMPARE(p.value(QStringLiteral("dim")).toInt(), 45);
        QCOMPARE(p.value(QStringLiteral("blur")).toInt(), 20);
        QCOMPARE(p.value(QStringLiteral("tint")).toInt(), 10);

        // The pure conversions agree both ways and clamp.
        const QVariantMap wire = backdrop::presentationToWire(
            { { QStringLiteral("dim"), 2.0 }, { QStringLiteral("tint"), 0.333 } });
        QCOMPARE(wire.value(QStringLiteral("dim")).toInt(), 100);
        QCOMPARE(wire.value(QStringLiteral("tint")).toInt(), 33);
        const QVariantMap back = backdrop::presentationFromWire(
            { { QStringLiteral("dim"), 250 }, { QStringLiteral("blur"), -3 },
              { QStringLiteral("tint"), QStringLiteral("lots") } });
        QCOMPARE(back.value(QStringLiteral("dim")).toDouble(), 1.0);
        QCOMPARE(back.value(QStringLiteral("blur")).toDouble(), 0.0);
        QCOMPARE(back.value(QStringLiteral("tint")).toDouble(), 0.25);
    }

    // ---- picking a picture --------------------------------------------------

    // An SVG is not a background as SVG: it is converted to a PNG at screen
    // scale first, and only that PNG is previewed and stored. Whatever it is
    // named, and however hostile, nothing SVG reaches the decoder (§6); the
    // magic-byte gate behind it is unchanged. A valid picture is re-encoded
    // with none of the original's text chunks (where a camera or an editor puts
    // GPS and comments).
    void svgIsConvertedAndMetadataNeverSurvives()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        ChatBackdropController backdrops;
        lightning::svgraster::hooks().inProcess = true;
        const auto reset = qScopeGuard([] { lightning::svgraster::hooks() = {}; });

        const QString svgPath = dir.filePath(QStringLiteral("not-really.png"));
        {
            QFile svg(svgPath);
            QVERIFY(svg.open(QIODevice::WriteOnly));
            svg.write("<svg xmlns='http://www.w3.org/2000/svg' width='40' "
                      "height='20'><rect width='40' height='20' fill='#c00'/>"
                      "<script>x</script></svg>");
        }
        QSignalSpy prepared(&backdrops, &ChatBackdropController::imagePrepared);
        QVariantMap r = backdrops.prepareImage(QUrl::fromLocalFile(svgPath));
        QVERIFY(!r.value(QStringLiteral("ok")).toBool());
        QVERIFY(r.value(QStringLiteral("pending")).toBool());
        QVERIFY(backdrops.prepared().isEmpty());
        if (lightning::svgraster::available()) {
            QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 10000);
            const QVariantMap done = prepared.first().first().toMap();
            QVERIFY(done.value(QStringLiteral("ok")).toBool());
            // At the 2560 screen-scale floor, not the 40x20 it declared.
            QCOMPARE(done.value(QStringLiteral("width")).toInt(), 2560);
            QCOMPARE(done.value(QStringLiteral("height")).toInt(), 1280);
            QVERIFY(!backdrops.prepared().isEmpty());
        }
        backdrops.discardPrepared();

        // A hostile SVG is refused with its reason and prepares nothing.
        const QString trap = dir.filePath(QStringLiteral("trap.svg"));
        {
            QFile svg(trap);
            QVERIFY(svg.open(QIODevice::WriteOnly));
            svg.write("<svg xmlns='http://www.w3.org/2000/svg' width='8' "
                      "height='8'><image href='/etc/hostname'/></svg>");
        }
        prepared.clear();
        QVERIFY(backdrops.prepareImage(QUrl::fromLocalFile(trap))
                    .value(QStringLiteral("pending")).toBool());
        QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 10000);
        QVERIFY(!prepared.first().first().toMap()
                     .value(QStringLiteral("ok")).toBool());
        QCOMPARE(backdrops.lastError(), QStringLiteral("svg_external_image"));
        QVERIFY(backdrops.svgMessage(QStringLiteral("external_image"))
                    .contains(QStringLiteral("links to other files")));
        QVERIFY(backdrops.prepared().isEmpty());

        // Discarding while a conversion runs drops its answer.
        prepared.clear();
        QVERIFY(backdrops.prepareImage(QUrl::fromLocalFile(svgPath))
                    .value(QStringLiteral("pending")).toBool());
        backdrops.discardPrepared();
        QTest::qWait(400);
        QCOMPARE(prepared.count(), 0);
        QVERIFY(backdrops.prepared().isEmpty());

        // A remote URL is never read.
        r = backdrops.prepareImage(QUrl(QStringLiteral("https://example.org/a.png")));
        QVERIFY(!r.value(QStringLiteral("ok")).toBool());

        QImage marked = solid(QColor(10, 120, 200, 128), 300, 200);
        marked.setText(QStringLiteral("Comment"), QStringLiteral("GPS-SECRET-48.85N"));
        r = backdrops.prepareImageForTesting(marked);
        QVERIFY2(r.value(QStringLiteral("ok")).toBool(),
                 qPrintable(r.value(QStringLiteral("error")).toString()));
        QCOMPARE(r.value(QStringLiteral("width")).toInt(), 300);

        // Read the encoded bytes back through the upload path.
        FakeBackdropClient client;
        client.roomList = { room(kRoom) };
        backdrops.setClient(&client);   // clears the session, so prepare again
        r = backdrops.prepareImageForTesting(marked);
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        backdrops.requestRoom(kRoom);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom, QVariantMap(),
                                             true, false);
        backdrops.setShared(kRoom, {});
        QVERIFY(!client.lastPath.isEmpty());
        QFile uploaded(client.lastPath);
        QVERIFY(uploaded.open(QIODevice::ReadOnly));
        const QByteArray bytes = uploaded.readAll();
        QVERIFY(bytes.startsWith("\x89PNG"));   // alpha -> PNG
        QVERIFY2(!bytes.contains("GPS-SECRET"),
                 "the original file's text chunk reached the upload");
        // The content names what was measured, not what the file claimed.
        QVERIFY(client.lastContent.contains(QStringLiteral("\"w\":300")));
        QVERIFY(client.lastContent.contains(QStringLiteral("image/png")));
    }

    void anOversizedPictureIsScaledToTheEdgeBound()
    {
        ChatBackdropController backdrops;
        const QVariantMap r =
            backdrops.prepareImageForTesting(solid(QColor(0, 0, 0, 200), 3000, 1000));
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        QCOMPARE(r.value(QStringLiteral("width")).toInt(), ChatBackdropController::kMaxEdge);
        QCOMPARE(r.value(QStringLiteral("height")).toInt(),
                 qRound(1000.0 * ChatBackdropController::kMaxEdge / 3000.0));
    }

    // ---- the scrim ------------------------------------------------------------

    // With nothing measured the scrim assumes pure white AND pure black under
    // the text. On every shipped preset the plan must be feasible, and every
    // opacity from the floor up must keep textPrimary, textSecondary and
    // textMuted at 4.5:1 over both, verified here independently. A fixed 50%
    // scrim of the plain ground must fail on every preset, which is what makes
    // the floor load-bearing.
    void theWorstCaseScrimKeepsEveryInkReadableOnEveryPreset()
    {
        const auto presets = presetPalettes(appTheme());
        QCOMPARE(presets.size(), 11);
        int checked = 0;
        int naiveFailures = 0;
        QStringList failures;
        for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
            const auto &p = it.value();
            const QColor ground = QColor::fromString(p.value(QStringLiteral("background")));
            const QList<QColor> inks{
                QColor::fromString(p.value(QStringLiteral("textPrimary"))),
                QColor::fromString(p.value(QStringLiteral("textSecondary"))),
                QColor::fromString(p.value(QStringLiteral("textMuted"))) };
            QVERIFY2(ground.isValid() && inks[0].isValid() && inks[1].isValid()
                         && inks[2].isValid(),
                     qPrintable(it.key()));
            const backdrop::ScrimPlan plan =
                backdrop::planScrim(ground, inks, backdrop::ImageStats(), 0.3);
            if (!plan.feasible || plan.floor > backdrop::kMaxScrimAlpha) {
                failures << QStringLiteral("%1: infeasible (floor %2)")
                                .arg(it.key()).arg(plan.floor);
                continue;
            }
            // Unmeasured: no tint is ever applied.
            QCOMPARE(plan.tint, 0.0);
            for (int k = int(std::lround(plan.floor * 100)); k <= 100; ++k) {
                const double a = k / 100.0;
                for (const QColor &under : { QColor(Qt::white), QColor(Qt::black) }) {
                    const QColor drawn = over(plan.color, a, under);
                    for (const QColor &ink : inks) {
                        if (ratio(ink, drawn) < 4.5) {
                            failures << QStringLiteral("%1: %2 on %3 at %4 = %5")
                                            .arg(it.key(), ink.name(), drawn.name())
                                            .arg(a).arg(ratio(ink, drawn));
                        }
                    }
                }
            }
            bool naiveFails = false;
            for (const QColor &under : { QColor(Qt::white), QColor(Qt::black) }) {
                const QColor drawn = over(ground, 0.5, under);
                for (const QColor &ink : inks)
                    naiveFails = naiveFails || ratio(ink, drawn) < 4.5;
            }
            naiveFailures += naiveFails ? 1 : 0;
            ++checked;
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
        QCOMPARE(checked, 11);
        QCOMPARE(naiveFailures, 11);
    }

    // Measuring buys visibility honestly: a mid-tone picture needs less scrim
    // than the worst case on every preset; a picture with large white and
    // black areas needs the worst case; specks under 1% are tolerated, 5% are
    // not. The measured floor is verified against every sample independently.
    void aMeasuredPictureGetsItsOwnFloorAndItIsVerifiedPerSample()
    {
        const auto presets = presetPalettes(appTheme());
        QCOMPARE(presets.size(), 11);
        const backdrop::ImageStats grey = backdrop::measure(solid(QColor(128, 128, 128)));
        QVERIFY(grey.measured);
        QImage halves = solid(Qt::white);
        {
            QPainter painter(&halves);
            painter.fillRect(0, 0, halves.width() / 2, halves.height(), Qt::black);
        }
        const backdrop::ImageStats split = backdrop::measure(halves);

        // 0.5% white specks on grey: tolerated. 5%: not.
        const auto speckled = [](double share) {
            QImage img = solid(QColor(128, 128, 128), 48, 48);
            const int n = int(std::lround(48 * 48 * share));
            for (int i = 0; i < n; ++i)
                img.setPixelColor((i * 7) % 48, (i * 13) % 48, Qt::white);
            return backdrop::measure(img);
        };
        const backdrop::ImageStats fewSpecks = speckled(0.005);
        const backdrop::ImageStats manySpecks = speckled(0.05);

        int checked = 0;
        QStringList failures;
        for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
            const auto &p = it.value();
            const QColor ground = QColor::fromString(p.value(QStringLiteral("background")));
            const QList<QColor> inks{
                QColor::fromString(p.value(QStringLiteral("textPrimary"))),
                QColor::fromString(p.value(QStringLiteral("textSecondary"))),
                QColor::fromString(p.value(QStringLiteral("textMuted"))) };
            const auto worst = backdrop::planScrim(ground, inks, backdrop::ImageStats(), 0);
            const auto mid = backdrop::planScrim(ground, inks, grey, 0);
            const auto both = backdrop::planScrim(ground, inks, split, 0);
            const auto few = backdrop::planScrim(ground, inks, fewSpecks, 0);
            const auto many = backdrop::planScrim(ground, inks, manySpecks, 0);
            if (!(mid.floor < worst.floor))
                failures << QStringLiteral("%1: grey %2 !< worst %3")
                                .arg(it.key()).arg(mid.floor).arg(worst.floor);
            if (std::abs(both.floor - worst.floor) > 0.011)
                failures << QStringLiteral("%1: black+white %2 != worst %3")
                                .arg(it.key()).arg(both.floor).arg(worst.floor);
            if (std::abs(few.floor - mid.floor) > 0.011)
                failures << QStringLiteral("%1: 0.5%% specks moved the floor %2 -> %3")
                                .arg(it.key()).arg(mid.floor).arg(few.floor);
            const bool dark = isDark(p.value(QStringLiteral("background")));
            // White specks are the worst case only under light ink.
            if (dark && !(many.floor > mid.floor + 0.02))
                failures << QStringLiteral("%1: 5%% specks ignored (%2 vs %3)")
                                .arg(it.key()).arg(many.floor).arg(mid.floor);
            // Independent per-sample verification of the grey plan.
            for (const QRgb px : grey.samples) {
                const QColor drawn = over(mid.color, mid.floor, QColor::fromRgb(px));
                for (const QColor &ink : inks) {
                    if (ratio(ink, drawn) < 4.5)
                        failures << QStringLiteral("%1: grey sample fails at floor")
                                        .arg(it.key());
                }
            }
            ++checked;
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
        QCOMPARE(checked, 11);
    }

    // The dominant colour may tint the scrim, never past kMaxTint, and the
    // tinted plan is verified like any other.
    void aTintedScrimStaysBoundedAndReadable()
    {
        const auto presets = presetPalettes(appTheme());
        const backdrop::ImageStats red = backdrop::measure(solid(QColor(200, 30, 40)));
        QVERIFY(red.dominant.isValid());
        QVERIFY(red.dominant.red() > red.dominant.blue());
        int checked = 0;
        for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
            const auto &p = it.value();
            const QList<QColor> inks{
                QColor::fromString(p.value(QStringLiteral("textPrimary"))),
                QColor::fromString(p.value(QStringLiteral("textSecondary"))),
                QColor::fromString(p.value(QStringLiteral("textMuted"))) };
            const auto plan = backdrop::planScrim(
                QColor::fromString(p.value(QStringLiteral("background"))), inks, red, 1.0);
            QVERIFY(plan.tint <= backdrop::kMaxTint + 1e-9);
            QVERIFY2(plan.feasible, qPrintable(it.key()));
            for (const QRgb px : red.samples) {
                const QColor drawn = over(plan.color, plan.floor, QColor::fromRgb(px));
                for (const QColor &ink : inks)
                    QVERIFY2(ratio(ink, drawn) >= 4.5, qPrintable(it.key()));
            }
            ++checked;
        }
        QCOMPARE(checked, 11);
    }

    // `dim` can only ADD scrim: whatever a shared event says, the drawn
    // opacity is never below the floor.
    void dimNeverGoesBelowTheFloor()
    {
        QCOMPARE(backdrop::scrimAlpha(0.6, 0.0), 0.6);
        QVERIFY(backdrop::scrimAlpha(0.6, 1.0) <= backdrop::kMaxScrimAlpha + 1e-9);
        QCOMPARE(backdrop::scrimAlpha(0.6, -5.0), 0.6);
        QCOMPARE(backdrop::scrimAlpha(0.6, std::nan("")), 0.6);
        QCOMPARE(backdrop::scrimAlpha(1.0, 0.0), 1.0);
        const QVariantMap p = backdrop::normalisePresentation(
            { { QStringLiteral("dim"), QStringLiteral("0.1") },
              { QStringLiteral("blur"), 9.0 },
              { QStringLiteral("fit"), QStringLiteral("stretch") },
              { QStringLiteral("evil"), 1 } });
        QCOMPARE(p.value(QStringLiteral("dim")).toDouble(), 0.2);   // string -> default
        QCOMPARE(p.value(QStringLiteral("blur")).toDouble(), 1.0);
        QCOMPARE(p.value(QStringLiteral("fit")).toString(), QStringLiteral("cover"));
        QVERIFY(!p.contains(QStringLiteral("evil")));
    }

    // ---- gradients --------------------------------------------------------------

    void gradientsAreSanitisedLikeColours()
    {
        const auto ok = [](const QVariantMap &spec) {
            return !CustomThemeStore::sanitizeGradient(spec).isEmpty();
        };
        const QVariantList two{ QStringLiteral("#112233"), QStringLiteral("#445566") };
        QVERIFY(ok({ { QStringLiteral("stops"), two } }));
        QVERIFY(!ok({ { QStringLiteral("stops"), QVariantList{ QStringLiteral("#112233") } } }));
        QVERIFY(!ok({ { QStringLiteral("stops"),
                        QVariantList{ QStringLiteral("#111111"), QStringLiteral("#222222"),
                                      QStringLiteral("#333333"), QStringLiteral("#444444") } } }));
        // Translucent or named colours are refused, never guessed at.
        QVERIFY(!ok({ { QStringLiteral("stops"),
                        QVariantList{ QStringLiteral("#80112233"), QStringLiteral("#445566") } } }));
        QVERIFY(!ok({ { QStringLiteral("stops"),
                        QVariantList{ QStringLiteral("red"), QStringLiteral("#445566") } } }));

        const QVariantMap s = CustomThemeStore::sanitizeGradient(
            QVariantMap{ { QStringLiteral("stops"), two }, { QStringLiteral("angle"), -90 },
                         { QStringLiteral("type"), QStringLiteral("conic") } });
        QCOMPARE(s.value(QStringLiteral("angle")).toInt(), 270);
        QCOMPARE(s.value(QStringLiteral("type")).toString(), QStringLiteral("linear"));
        QCOMPARE(s.value(QStringLiteral("stops")).toStringList().first(),
                 QStringLiteral("#112233"));

        // Only surface roles; ink never carries a gradient.
        const QVariantMap all = CustomThemeStore::sanitizeGradients(
            { { QStringLiteral("background"), QVariantMap{ { QStringLiteral("stops"), two } } },
              { QStringLiteral("textPrimary"), QVariantMap{ { QStringLiteral("stops"), two } } },
              { QStringLiteral("nonsense"), QVariantMap{ { QStringLiteral("stops"), two } } } });
        QCOMPARE(all.keys(), QStringList{ QStringLiteral("background") });
        QVERIFY(CustomThemeStore::roleTakesGradient(QStringLiteral("sidebar")));
        QVERIFY(!CustomThemeStore::roleTakesGradient(QStringLiteral("ownBubbleText")));
    }

    // A theme saved before gradients existed loads unchanged; a gradient
    // survives save, reload and export/import; a share with only gradients is
    // a real theme.
    void gradientsAreBackwardsCompatibleAndRoundTrip()
    {
        {
            QSettings raw;
            raw.setValue(QStringLiteral("appearance/customThemeList"),
                         QStringLiteral("[{\"id\":\"1\",\"name\":\"Old\",\"base\":2,"
                                        "\"colors\":{\"background\":\"#101010\"}}]"));
            raw.setValue(QStringLiteral("appearance/customThemeActive"), QStringLiteral("1"));
            raw.sync();
        }
        SettingsManager settings;
        CustomThemeStore store(&settings);
        QCOMPARE(store.colors().value(QStringLiteral("background")).toString(),
                 QStringLiteral("#101010"));
        QVERIFY(store.gradients().isEmpty());

        const QVariantMap spec{ { QStringLiteral("stops"),
                                  QVariantList{ QStringLiteral("#101010"),
                                                QStringLiteral("#202020") } },
                                { QStringLiteral("angle"), 135 } };
        QVERIFY(store.setGradient(QStringLiteral("background"), spec));
        QVERIFY(!store.setGradient(QStringLiteral("textPrimary"), spec));
        CustomThemeStore reloaded(&settings);
        QCOMPARE(reloaded.gradients().value(QStringLiteral("background")).toMap()
                     .value(QStringLiteral("angle")).toInt(), 135);
        QCOMPARE(reloaded.colors().value(QStringLiteral("background")).toString(),
                 QStringLiteral("#101010"));

        const QString exported = reloaded.exportTheme(reloaded.activeThemeId());
        QVERIFY(exported.contains(QStringLiteral("gradients")));
        QCOMPARE(reloaded.importTheme(exported), QString());
        QCOMPARE(reloaded.gradients().value(QStringLiteral("background")).toMap()
                     .value(QStringLiteral("stops")).toStringList().size(), 2);

        const QString onlyGradient = QStringLiteral(
            "{\"lightning_theme\":1,\"name\":\"G\",\"base\":2,\"colors\":{},"
            "\"gradients\":{\"sidebar\":{\"stops\":[\"#000000\",\"#111111\"]}}}");
        QCOMPARE(reloaded.importTheme(onlyGradient), QString());
        reloaded.resetGradient(QStringLiteral("sidebar"));
        QVERIFY(!reloaded.gradients().contains(QStringLiteral("sidebar")));
    }

    // A gradient is graded at its worst stop: the flat palette passes, the
    // same background with one dark stop under dark ink fails, and the row
    // names that stop.
    void readabilityUsesTheWorstStop()
    {
        const auto presets = presetPalettes(appTheme());
        const QString danger = colorLiteral(appTheme(), QStringLiteral("_accentDanger"));
        const QVariantMap light = resolvedPalette(presets.value(QStringLiteral("Lightning Light")),
                                                  danger);
        SettingsManager settings;
        CustomThemeStore store(&settings);
        QVERIFY(store.audit(light).isEmpty());
        QCOMPARE(store.auditWithGradients(light, {}).size(), store.audit(light).size());

        const QVariantMap gradients{
            { QStringLiteral("background"),
              QVariantMap{ { QStringLiteral("stops"),
                             QVariantList{ light.value(QStringLiteral("background")),
                                           QStringLiteral("#303030") } } } } };
        const QVariantList bad = store.auditWithGradients(light, gradients);
        QVERIFY(!bad.isEmpty());
        bool namedStop = false;
        for (const QVariant &row : bad) {
            const QVariantMap m = row.toMap();
            if (m.value(QStringLiteral("bg")).toString() == QLatin1String("background"))
                namedStop = namedStop || m.value(QStringLiteral("stop")).toInt() == 1;
        }
        QVERIFY(namedStop);
    }

    // "Depth" on every preset: background, room list and rail all get their
    // two stops and every readability check still passes at the worst stop.
    // Pointing the shift TOWARDS the ink instead must fail somewhere, or this
    // test could not see a wrong direction.
    void depthPassesEveryCheckOnEveryPreset()
    {
        const QString qml = appTheme();
        const auto presets = presetPalettes(qml);
        QCOMPARE(presets.size(), 11);
        const QString danger = colorLiteral(qml, QStringLiteral("_accentDanger"));
        SettingsManager settings;
        CustomThemeStore store(&settings);

        const auto depthFor = [](const QVariantMap &palette, bool towardsInk) {
            QVariantMap out;
            const bool dark = isDark(palette.value(QStringLiteral("background")).toString());
            for (const QString role : { QStringLiteral("background"), QStringLiteral("sidebar"),
                                        QStringLiteral("rail") }) {
                const QColor base = QColor::fromString(palette.value(role).toString());
                const QStringList stops =
                    backdrop::depthStops(base, towardsInk ? !dark : dark);
                out.insert(role, QVariantMap{ { QStringLiteral("stops"), stops },
                                              { QStringLiteral("angle"), 180 } });
            }
            return out;
        };

        int checked = 0;
        int wrongDirectionFailures = 0;
        QStringList failures;
        for (auto it = presets.constBegin(); it != presets.constEnd(); ++it) {
            const QVariantMap palette = resolvedPalette(it.value(), danger);
            const QVariantMap gradients = depthFor(palette, false);
            QCOMPARE(gradients.size(), 3);
            for (const QVariant &row : store.auditWithGradients(palette, gradients)) {
                const QVariantMap m = row.toMap();
                failures << QStringLiteral("%1: %2 on %3 = %4 (stop %5)")
                                .arg(it.key(), m.value(QStringLiteral("fg")).toString(),
                                     m.value(QStringLiteral("bg")).toString())
                                .arg(m.value(QStringLiteral("value")).toDouble())
                                .arg(m.value(QStringLiteral("stop")).toInt());
            }
            if (!store.auditWithGradients(palette, depthFor(palette, true)).isEmpty())
                ++wrongDirectionFailures;
            ++checked;
        }
        QVERIFY2(failures.isEmpty(), qPrintable(failures.join(QLatin1Char('\n'))));
        QCOMPARE(checked, 11);
        QVERIFY2(wrongDirectionFailures > 0,
                 "a depth shift towards the ink passed everywhere: the audit "
                 "cannot see the direction this test exists to pin");
    }

    void depthStopsMoveAwayFromTheInk()
    {
        const QColor darkBase(0x1B, 0x24, 0x2F);
        const QStringList d = backdrop::depthStops(darkBase, true);
        QCOMPARE(d.size(), 2);
        QCOMPARE(d.first(), darkBase.name().toUpper());
        QVERIFY(backdrop::lstar(QColor::fromString(d.last())) < backdrop::lstar(darkBase) - 3.0);
        const QColor lightBase(0xD3, 0xE1, 0xF2);
        const QStringList l = backdrop::depthStops(lightBase, false);
        QCOMPARE(l.last(), lightBase.name().toUpper());
        QVERIFY(backdrop::lstar(QColor::fromString(l.first())) > backdrop::lstar(lightBase) + 3.0);
    }
};

QTEST_MAIN(ChatBackdropTest)
#include "ChatBackdropTest.moc"
