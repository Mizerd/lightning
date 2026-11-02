
#include "app/CustomThemeStore.h"
#include "app/SettingsManager.h"
#include "support/BadgeColourRules.h"
#include "backdrop/BackdropMath.h"
#include "backdrop/ChatBackdropController.h"
#include "matrix/MockMatrixClient.h"
#include "matrix/RoomInfo.h"
#include "media/StagedImageStore.h"
#include "media/AnimatedImageSniff.h"
#include "media/SvgRasterJob.h"

#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QImageReader>
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

QHash<QString, QString> elementGroupsTable(const QString &qml)
{
    QHash<QString, QString> out;
    static const QRegularExpression block(
        QStringLiteral("readonly\\s+property\\s+var\\s+elementGroups:\\s*\\(\\{(.*?)\\}\\)"),
        QRegularExpression::DotMatchesEverythingOption);
    const auto m = block.match(qml);
    if (!m.hasMatch())
        return out;
    static const QRegularExpression pair(
        QStringLiteral("(\\w+)\\s*:\\s*\"(\\w+)\""));
    auto it = pair.globalMatch(m.captured(1));
    while (it.hasNext()) {
        const auto e = it.next();
        out.insert(e.captured(1), e.captured(2));
    }
    return out;
}

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
    fallback("unreadBadge", raw.value(QStringLiteral("accent")));
    QHash<QString, QString> groups = elementGroupsTable(appTheme());
    groups.insert(QStringLiteral("embedSurface"), QStringLiteral("cardElevated"));
    groups.insert(QStringLiteral("roomSelected"), QStringLiteral("selected"));
    groups.insert(QStringLiteral("popoverSurface"), QStringLiteral("surface"));
    for (auto it = groups.constBegin(); it != groups.constEnd(); ++it) {
        if (p.contains(it.value()))
            p.insert(it.key(), p.value(it.value()));
    }
    badgerules::addDerived(p);
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

QByteArray makeGif(int w, int h, int frames, int padding = 0)
{
    QByteArray g("GIF89a");
    const auto le16 = [&g](int v) {
        g.append(char(v & 0xff));
        g.append(char((v >> 8) & 0xff));
    };
    le16(w);
    le16(h);
    g.append(char(0x80));
    g.append('\0');
    g.append('\0');
    g.append(QByteArray("\xff\x00\x00\x00\x00\xff", 6));
    g.append(QByteArray("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19));
    if (padding > 0) {
        g.append(QByteArray("\x21\xfe", 2));
        for (int left = padding; left > 0; left -= 255) {
            const int n = qMin(left, 255);
            g.append(char(n));
            g.append(QByteArray(n, 'x'));
        }
        g.append('\0');
    }
    for (int i = 0; i < frames; ++i) {
        g.append(QByteArray("\x21\xf9\x04\x00\x0a\x00\x00\x00", 8));
        g.append(char(0x2c));
        le16(0);
        le16(0);
        le16(1);
        le16(1);
        g.append('\0');
        g.append(char(0x02));
        g.append(QByteArray("\x02\x44\x01\x00", 4));
    }
    g.append(char(0x3b));
    return g;
}

QByteArray solidFramesGif(int n, const QList<int> &colours, int delayCs = 10)
{
    QByteArray g("GIF89a");
    const auto le16 = [&g](int v) {
        g.append(char(v & 0xff));
        g.append(char((v >> 8) & 0xff));
    };
    le16(n);
    le16(n);
    g.append(char(0x80));
    g.append('\0');
    g.append('\0');
    g.append(QByteArray("\x00\x00\x00\xff\xff\xff", 6));
    g.append(QByteArray("\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00", 19));
    for (const int colour : colours) {
        g.append(QByteArray("\x21\xf9\x04\x00", 4));
        g.append(char(delayCs & 0xff));
        g.append(char((delayCs >> 8) & 0xff));
        g.append(QByteArray("\x00\x00", 2));
        g.append(char(0x2c));
        le16(0);
        le16(0);
        le16(n);
        le16(n);
        g.append('\0');
        g.append(char(0x02));
        QList<int> codes;
        for (int i = 0; i < n * n; ++i) {
            if (i % 2 == 0)
                codes.append(4);
            codes.append(colour);
        }
        codes.append(5);
        QByteArray data;
        quint32 acc = 0;
        int bits = 0;
        for (const int code : codes) {
            acc |= quint32(code) << bits;
            bits += 3;
            while (bits >= 8) {
                data.append(char(acc & 0xff));
                acc >>= 8;
                bits -= 8;
            }
        }
        if (bits > 0)
            data.append(char(acc & 0xff));
        for (int off = 0; off < data.size(); off += 255) {
            const QByteArray chunk = data.mid(off, 255);
            g.append(char(chunk.size()));
            g.append(chunk);
        }
        g.append('\0');
    }
    g.append(char(0x3b));
    return g;
}

bool canDecode(const QByteArray &bytes)
{
    QByteArray copy = bytes;
    QBuffer buffer(&copy);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    return !reader.read().isNull();
}

}

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
        QCOMPARE(r.scopeId, kSpace);
        in.sharedSpaces[0].second.clear();
        r = backdrop::resolve(kRoom, in);
        QCOMPARE(r.scopeId, kParent);
        in.sharedSpaces.clear();
        QCOMPARE(backdrop::resolve(kRoom, in).source, QStringLiteral("personal-default"));
        in.personalDefault.clear();
        QCOMPARE(backdrop::resolve(kRoom, in).source, QStringLiteral("none"));
    }

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
        children[QStringLiteral("!b")].append(QStringLiteral("!root"));
        const QStringList chain =
            backdrop::spaceChain(kRoom, children, order, QStringLiteral("!b"));
        QVERIFY(chain.size() <= backdrop::kMaxSpaceDepth + 1);
        QCOMPARE(chain.first(), QStringLiteral("!b"));
        QCOMPARE(backdrop::spaceChain(QStringLiteral("!lonely"), children, order, {}),
                 QStringList());
    }

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
        QCOMPARE(client.asks.size(), 3);
        backdrops.requestRoom(kRoom);
        QCOMPARE(client.asks.size(), 3);

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

        Q_EMIT client.loggedOut();
        Q_EMIT client.roomBackgroundReceived(op, kRoom,
                                             shared(QStringLiteral("mxc://e.org/x")),
                                             true, false);
        QCOMPARE(backdrops.sharedFor(kRoom), QVariantMap());
        QVERIFY(!backdrops.canSetShared(kRoom));
    }

    void aChangeInSyncRereadsTheOpenRoomAndItsSpaceAtOnce()
    {
        FakeBackdropClient client;
        client.roomList = { room(kRoom), room(kSpace, true, { kRoom }) };
        ChatBackdropController backdrops;
        backdrops.setClient(&client);
        backdrops.requestRoom(kRoom);
        QCOMPARE(client.asks.size(), 2);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             QVariantMap(), false, false);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kSpace), kSpace,
                                             QVariantMap(), false, false);
        QCOMPARE(backdrops.backdropFor(kRoom).value(QStringLiteral("source")).toString(),
                 QStringLiteral("none"));

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

        const qsizetype before = client.asks.size();
        Q_EMIT client.roomBackgroundChanged(QStringLiteral("!elsewhere:example.org"));
        QCOMPARE(client.asks.size(), before);

        Q_EMIT client.roomBackgroundChanged(kRoom);
        QCOMPARE(client.asks.size(), before + 1);
        Q_EMIT client.roomBackgroundChanged(kRoom);
        QCOMPARE(client.asks.size(), before + 1);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom,
                                             QVariantMap(), false, false);
        QCOMPARE(client.asks.size(), before + 2);
        QCOMPARE(client.asks.last().scope, kRoom);

        Q_EMIT client.loggedOut();
        const qsizetype afterSignOut = client.asks.size();
        Q_EMIT client.roomBackgroundChanged(kRoom);
        QCOMPARE(client.asks.size(), afterSignOut);
    }

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
                                             true,true);
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
        QVERIFY(backdrops.showShared());
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
            QCOMPARE(done.value(QStringLiteral("width")).toInt(), 2560);
            QCOMPARE(done.value(QStringLiteral("height")).toInt(), 1280);
            QVERIFY(!backdrops.prepared().isEmpty());
        }
        backdrops.discardPrepared();

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

        prepared.clear();
        QVERIFY(backdrops.prepareImage(QUrl::fromLocalFile(svgPath))
                    .value(QStringLiteral("pending")).toBool());
        backdrops.discardPrepared();
        QTest::qWait(400);
        QCOMPARE(prepared.count(), 0);
        QVERIFY(backdrops.prepared().isEmpty());

        r = backdrops.prepareImage(QUrl(QStringLiteral("https://example.org/a.png")));
        QVERIFY(!r.value(QStringLiteral("ok")).toBool());

        QImage marked = solid(QColor(10, 120, 200, 128), 300, 200);
        marked.setText(QStringLiteral("Comment"), QStringLiteral("GPS-SECRET-48.85N"));
        r = backdrops.prepareImageForTesting(marked);
        QVERIFY2(r.value(QStringLiteral("ok")).toBool(),
                 qPrintable(r.value(QStringLiteral("error")).toString()));
        QCOMPARE(r.value(QStringLiteral("width")).toInt(), 300);

        FakeBackdropClient client;
        client.roomList = { room(kRoom) };
        backdrops.setClient(&client);
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
        QVERIFY(bytes.startsWith("\x89PNG"));
        QVERIFY2(!bytes.contains("GPS-SECRET"),
                 "the original file's text chunk reached the upload");
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

    void anAnimatedGifIsKeptWithItsMetadataStripped()
    {
        const QByteArray gif = makeGif(64, 32, 3, 600);
        if (!canDecode(gif))
            QSKIP("this Qt cannot decode the GIF fixture");
        FakeBackdropClient client;
        client.roomList = { room(kRoom) };
        ChatBackdropController backdrops;
        backdrops.setClient(&client);
        const QVariantMap r = backdrops.prepareBytesForTesting(gif);
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        QVERIFY(r.value(QStringLiteral("animated")).toBool());
        QCOMPARE(r.value(QStringLiteral("width")).toInt(), 64);
        QCOMPARE(r.value(QStringLiteral("height")).toInt(), 32);

        backdrops.requestRoom(kRoom);
        Q_EMIT client.roomBackgroundReceived(client.opFor(kRoom), kRoom, QVariantMap(),
                                             true, false);
        backdrops.setShared(kRoom, {});
        QVERIFY(!client.lastPath.isEmpty());
        QVERIFY(client.lastPath.endsWith(QStringLiteral(".gif")));
        QFile uploaded(client.lastPath);
        QVERIFY(uploaded.open(QIODevice::ReadOnly));
        const QByteArray bytes = uploaded.readAll();
        QVERIFY(bytes.startsWith("GIF89a"));
        QVERIFY2(!bytes.contains("xxxxxxxx"), "the GIF comment reached the upload");
        QVERIFY(client.lastContent.contains(QStringLiteral("image/gif")));
    }

    void onlyAnAnimationWithinTheBoundsIsKept()
    {
        ChatBackdropController backdrops;
        QVariantMap r = backdrops.prepareImageForTesting(solid(QColor(10, 20, 30), 40, 20));
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        QVERIFY(!r.value(QStringLiteral("animated")).toBool());

        const QByteArray wide = makeGif(ChatBackdropController::kMaxEdge + 1, 40, 2);
        if (canDecode(wide)) {
            r = backdrops.prepareBytesForTesting(wide);
            QVERIFY(r.value(QStringLiteral("ok")).toBool());
            QVERIFY2(!r.value(QStringLiteral("animated")).toBool(),
                     "a canvas past kMaxEdge must stay a first-frame still");
            QVERIFY(r.value(QStringLiteral("width")).toInt()
                    <= ChatBackdropController::kMaxEdge);
        }
        const QByteArray one = makeGif(32, 32, 1);
        if (canDecode(one)) {
            r = backdrops.prepareBytesForTesting(one);
            QVERIFY(r.value(QStringLiteral("ok")).toBool());
            QVERIFY(!r.value(QStringLiteral("animated")).toBool());
        }
        const QByteArray heavy = makeGif(32, 32, 2,
            int(ChatBackdropController::kMaxAnimatedBytes) + 1024);
        if (canDecode(heavy)) {
            r = backdrops.prepareBytesForTesting(heavy);
            QVERIFY(!r.value(QStringLiteral("animated")).toBool());
        }
    }

    void animationStatsCoverEveryFrame()
    {
        const QImage white = solid(QColor(255, 255, 255), 16, 16);
        const QImage black = solid(QColor(0, 0, 0), 16, 16);
        const backdrop::ImageStats first = backdrop::measure(white);
        const backdrop::ImageStats both = backdrop::measureFrames({ white, black });
        QVERIFY(first.measured);
        QVERIFY(both.measured);
        QCOMPARE(both.samples.size(), first.samples.size() * 2);
        QVERIFY(both.samples.contains(QColor(Qt::white).rgb()));
        QVERIFY(both.samples.contains(QColor(Qt::black).rgb()));
        QVERIFY(!backdrop::measureFrames({}).measured);
        QVERIFY(!backdrop::measureFrames({ QImage() }).measured);
    }

    void aLaterWhiteFrameRaisesTheScrim()
    {
        const QByteArray gif = solidFramesGif(8, { 0, 1 });
        if (!canDecode(gif))
            QSKIP("this Qt cannot decode the GIF fixture");
        const auto alphaOf = [](ChatBackdropController &b, const QVariantMap &r) {
            return b.scrimFor(r.value(QStringLiteral("statsKey")).toString(),
                              QColor(0x14, 0x16, 0x1a), QColor(0xf0, 0xf0, 0xf0),
                              QColor(0xc0, 0xc0, 0xc0), QColor(0x9a, 0x9a, 0x9a),
                              0.0, 0.0)
                .value(QStringLiteral("alpha")).toDouble();
        };
        ChatBackdropController backdrops;
        const QVariantMap animated = backdrops.prepareBytesForTesting(gif);
        QVERIFY(animated.value(QStringLiteral("ok")).toBool());
        QVERIFY(animated.value(QStringLiteral("animated")).toBool());
        const double both = alphaOf(backdrops, animated);
        const QVariantMap white =
            backdrops.prepareImageForTesting(solid(QColor(255, 255, 255), 8, 8));
        const double whiteOnly = alphaOf(backdrops, white);
        const QVariantMap black =
            backdrops.prepareImageForTesting(solid(QColor(0, 0, 0), 8, 8));
        const double blackOnly = alphaOf(backdrops, black);
        QVERIFY2(whiteOnly > blackOnly + 0.05, "fixture: white must need more scrim");
        QVERIFY2(both >= whiteOnly - 1e-6,
                 "the white frame was not measured: scrim planned for frame 0");
    }

    void aFlashingOrEndlessAnimationStaysAStill()
    {
        ChatBackdropController backdrops;
        const QByteArray flashing = solidFramesGif(8, { 0, 1, 0, 1 }, 3);
        if (!canDecode(flashing))
            QSKIP("this Qt cannot decode the GIF fixture");
        QVERIFY(!backdrops.prepareBytesForTesting(flashing)
                     .value(QStringLiteral("animated")).toBool());
        QList<int> many;
        for (int i = 0; i < 320; ++i)
            many.append(i % 2);
        QVERIFY(!backdrops.prepareBytesForTesting(solidFramesGif(4, many))
                     .value(QStringLiteral("animated")).toBool());
        for (const int cs : { 0, 1, 5, 10 }) {
            QVERIFY2(backdrops.prepareBytesForTesting(solidFramesGif(8, { 0, 1, 0, 1 }, cs))
                         .value(QStringLiteral("animated")).toBool(),
                     qPrintable(QStringLiteral("a %1 cs GIF must be kept").arg(cs)));
        }
    }

    void theGifPacingIsTheLastExtensionBeforeEachImage()
    {
        namespace sniff = lightning::animsniff;
        const QByteArray fast = solidFramesGif(8, { 0, 1, 0, 1 }, 3);
        const QByteArray gce("\x21\xf9\x04\x00\x03\x00\x00\x00", 8);
        const QByteArray slow("\x21\xf9\x04\x00\x64\x00\x00\x00", 8);
        QVERIFY(fast.contains(gce));
        QByteArray diluted = fast;
        diluted.replace(gce, slow + slow + gce);
        QVERIFY(sniff::timingTooBusy(sniff::animationTiming(fast)));
        const sniff::AnimationTiming timing = sniff::animationTiming(diluted);
        QCOMPARE(timing.frames, 4);
        QCOMPARE(timing.counted, 4);
        QVERIFY2(sniff::timingTooBusy(timing), "dummy extensions diluted the pacing");
        QByteArray trailing = fast;
        trailing.chop(1);
        for (int i = 0; i < 12; ++i)
            trailing.append(slow);
        trailing.append(char(0x3b));
        QVERIFY2(sniff::timingTooBusy(sniff::animationTiming(trailing)),
                 "extensions after the last frame diluted the pacing");
        QVERIFY(!sniff::timingTooBusy(sniff::animationTiming(
            solidFramesGif(8, { 0, 1, 0, 1 }, 10))));
    }

    void theWebpPacingStopsAtTheRiffEnd()
    {
        namespace sniff = lightning::animsniff;
        const auto le32 = [](quint32 v) {
            QByteArray b;
            for (int i = 0; i < 4; ++i)
                b.append(char((v >> (8 * i)) & 0xff));
            return b;
        };
        const auto anmf = [&](int ms) {
            QByteArray payload(12, '\0');
            payload.append(char(ms & 0xff));
            payload.append(char((ms >> 8) & 0xff));
            payload.append(char((ms >> 16) & 0xff));
            payload.append('\0');
            return QByteArray("ANMF") + le32(16) + payload;
        };
        const auto webp = [&](const QList<int> &inside, const QList<int> &after) {
            QByteArray body("WEBP");
            body += QByteArray("VP8X") + le32(10) + QByteArray("\x02\0\0\0", 4)
                    + QByteArray("\x07\0\0\x07\0\0", 6);
            body += QByteArray("ANIM") + le32(6) + QByteArray(6, '\0');
            for (const int ms : inside)
                body += anmf(ms);
            QByteArray out = QByteArray("RIFF") + le32(quint32(body.size())) + body;
            for (const int ms : after)
                out += anmf(ms);
            return out;
        };
        QVERIFY(sniff::isAnimatedWebp(webp({ 100, 100 }, {})));
        const sniff::AnimationTiming outside =
            sniff::animationTiming(webp({ 100, 100 }, { 20, 20, 20, 20, 20, 20, 20, 20, 20, 20 }));
        QCOMPARE(outside.frames, 2);
        QVERIFY2(!sniff::timingTooBusy(outside),
                 "chunks past the RIFF size were counted");
        QVERIFY(sniff::timingTooBusy(sniff::animationTiming(webp({ 20, 20, 20 }, {}))));
        QVERIFY2(sniff::timingTooBusy(sniff::animationTiming(webp({ 0, 0, 0 }, {}))),
                 "a 0 ms WebP duration must count as fast");
        QVERIFY(!sniff::timingTooBusy(sniff::animationTiming(webp({ 100, 80, 60 }, {}))));
    }

    void anAnimationTheSamplerCannotFullySeeIsNotPlannedFromItsFirstPart()
    {
        QList<int> colours;
        for (int i = 0; i < 60; ++i)
            colours.append(i < 48 ? 0 : 1);
        const QByteArray gif = solidFramesGif(8, colours);
        if (!canDecode(gif))
            QSKIP("this Qt cannot decode the GIF fixture");
        ChatBackdropController backdrops;
        QVERIFY2(!backdrops.prepareBytesForTesting(gif)
                      .value(QStringLiteral("animated")).toBool(),
                 "a cut-short sample must not keep the picture animated");
        const auto alphaOf = [](const backdrop::ImageStats &stats) {
            const backdrop::ScrimPlan plan = backdrop::planScrim(
                QColor(0x14, 0x16, 0x1a),
                { QColor(0xf0, 0xf0, 0xf0), QColor(0xc0, 0xc0, 0xc0),
                  QColor(0x9a, 0x9a, 0x9a) },
                stats, 0.0);
            return backdrop::scrimAlpha(plan.floor, 0.0);
        };
        const double measured = alphaOf(backdrops.measureBytesForTesting(gif));
        const double whiteOnly = alphaOf(backdrop::measure(solid(QColor(255, 255, 255), 8, 8)));
        const double blackOnly = alphaOf(backdrop::measure(solid(QColor(0, 0, 0), 8, 8)));
        QVERIFY2(whiteOnly > blackOnly + 0.05, "fixture: white must need more scrim");
        QVERIFY2(measured >= whiteOnly - 1e-6,
                 "the white frames after the cut were not planned for");
    }

    void onlyItsOwnStagedTokensAreOwned()
    {
        StagedImageStore store;
        ChatBackdropController backdrops;
        backdrops.setStagedImages(&store);
        const QVariantMap r = backdrops.prepareImageForTesting(solid(QColor(9, 9, 9), 8, 8));
        QVERIFY(r.value(QStringLiteral("ok")).toBool());
        const QString url = r.value(QStringLiteral("imageUrl")).toString();
        const QString prefix = QStringLiteral("image://lightning-staged/");
        QVERIFY(url.startsWith(prefix));
        const QString mine = url.mid(prefix.size());
        QVERIFY(backdrops.ownsStagedToken(mine));
        const QString foreign = store.add(QByteArray("GIF89a-an-outgoing-attachment"));
        QVERIFY(!foreign.isEmpty());
        QVERIFY(!backdrops.ownsStagedToken(foreign));
        QVERIFY(!backdrops.ownsStagedToken(QString()));
        QVERIFY(!backdrops.ownsStagedToken(QStringLiteral("nope")));
        backdrops.discardPrepared();
        QVERIFY(!backdrops.ownsStagedToken(mine));
    }

    void theBackdropAnimatesOnlyUnderEveryGate()
    {
        QFile f(QStringLiteral(LIGHTNING_SOURCE_DIR "/qml/ChatBackdrop.qml"));
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString qml = QString::fromUtf8(f.readAll());
        QVERIFY(qml.contains(QStringLiteral("AnimatedImage {")));
        QVERIFY(qml.contains(QStringLiteral("AnimationFileHold {}")));
        const int wants = qml.indexOf(QStringLiteral("readonly property bool _wantsMotion"));
        QVERIFY(wants > 0);
        const QString binding = qml.mid(wants, 420);
        QVERIFY(binding.contains(QStringLiteral("_motionMode === 0")));
        QVERIFY(binding.contains(QStringLiteral("!AppTheme.reducedMotion")));
        QVERIFY(binding.contains(QStringLiteral("!root.blurOn")));
        QVERIFY(binding.contains(QStringLiteral("_windowShown")));
        QVERIFY(binding.contains(QStringLiteral("picture.status === Image.Ready")));
        QVERIFY(qml.contains(QStringLiteral("active: root._wantsMotion")));
        QVERIFY(qml.contains(QStringLiteral("id: picture")));
        QVERIFY(qml.contains(QStringLiteral("opacity: root.motionShown ? 0 : 1")));
        QVERIFY(qml.contains(QStringLiteral("source: root._motionSrc")));
        QVERIFY(!qml.contains(QStringLiteral("motionImage.source =")));
    }

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
            if (dark && !(many.floor > mid.floor + 0.02))
                failures << QStringLiteral("%1: 5%% specks ignored (%2 vs %3)")
                                .arg(it.key()).arg(many.floor).arg(mid.floor);
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
        QCOMPARE(p.value(QStringLiteral("dim")).toDouble(), 0.2);
        QCOMPARE(p.value(QStringLiteral("blur")).toDouble(), 1.0);
        QCOMPARE(p.value(QStringLiteral("fit")).toString(), QStringLiteral("cover"));
        QVERIFY(!p.contains(QStringLiteral("evil")));
    }

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

        const QVariantMap all = CustomThemeStore::sanitizeGradients(
            { { QStringLiteral("background"), QVariantMap{ { QStringLiteral("stops"), two } } },
              { QStringLiteral("textPrimary"), QVariantMap{ { QStringLiteral("stops"), two } } },
              { QStringLiteral("nonsense"), QVariantMap{ { QStringLiteral("stops"), two } } } });
        QCOMPARE(all.keys(), QStringList{ QStringLiteral("background") });
        QVERIFY(CustomThemeStore::roleTakesGradient(QStringLiteral("sidebar")));
        QVERIFY(!CustomThemeStore::roleTakesGradient(QStringLiteral("ownBubbleText")));
    }

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

    void gradientStopPositionsAreOptionalAndRoundTrip()
    {
        const QVariantList three{ QStringLiteral("#101010"), QStringLiteral("#202020"),
                                  QStringLiteral("#303030") };
        const QVariantList two{ QStringLiteral("#101010"), QStringLiteral("#202020") };
        const auto positionsOf = [&](const QVariant &positions, const QVariantList &stops) {
            return CustomThemeStore::sanitizeGradient(
                QVariantMap{ { QStringLiteral("stops"), stops },
                             { QStringLiteral("positions"), positions } })
                .value(QStringLiteral("positions")).toList();
        };
        const QVariantList kept = positionsOf(QVariantList{ 0.0, 0.3141, 1.0 }, three);
        QCOMPARE(kept.size(), 3);
        QCOMPARE(kept.at(1).toDouble(), 0.31);
        QVERIFY(positionsOf(QVariantList{ 0.0, 0.5, 1.0 }, three).isEmpty());
        QVERIFY(positionsOf(QVariant(), three).isEmpty());
        QVERIFY(positionsOf(QVariantList{ 0.0, 1.0 }, three).isEmpty());
        QVERIFY(positionsOf(QVariantList{ 0.0, 0.8, 0.4 }, three).isEmpty());
        QVERIFY(positionsOf(QVariantList{ -0.1, 0.5, 1.2 }, three).isEmpty());
        QVERIFY(positionsOf(QVariantList{ QStringLiteral("0"), 0.2, 1.0 }, three).isEmpty());
        QVERIFY(positionsOf(QVariantList{ 0.0, 1.0 }, two).isEmpty());
        QCOMPARE(positionsOf(QVariantList{ 0.2, 0.9 }, two).size(), 2);
        const QVariantMap plain = CustomThemeStore::sanitizeGradient(
            QVariantMap{ { QStringLiteral("stops"), three } });
        QCOMPARE(plain.keys(), (QStringList{ QStringLiteral("angle"),
                                             QStringLiteral("stops"),
                                             QStringLiteral("type") }));

        SettingsManager settings;
        CustomThemeStore store(&settings);
        QVERIFY(store.setGradient(
            QStringLiteral("sidebar"),
            QVariantMap{ { QStringLiteral("stops"), three },
                         { QStringLiteral("positions"), QVariantList{ 0.1, 0.35, 0.9 } } }));
        CustomThemeStore reloaded(&settings);
        QCOMPARE(reloaded.gradients().value(QStringLiteral("sidebar")).toMap()
                     .value(QStringLiteral("positions")).toList().at(1).toDouble(), 0.35);
        const QString exported = reloaded.exportTheme(reloaded.activeThemeId());
        QVERIFY(exported.contains(QStringLiteral("positions")));
        reloaded.resetGradient(QStringLiteral("sidebar"));
        QCOMPARE(reloaded.importTheme(exported), QString());
        QCOMPARE(reloaded.gradients().value(QStringLiteral("sidebar")).toMap()
                     .value(QStringLiteral("positions")).toList().size(), 3);
    }

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
            if (m.value(QStringLiteral("bg")).toString()
                == QLatin1String("timelineBackground"))
                namedStop = namedStop || m.value(QStringLiteral("stop")).toInt() == 1;
        }
        QVERIFY(namedStop);
    }

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
