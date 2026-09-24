// v0.6.5 (SPEC §1q): behavior + source-scan proof for the redesigned
// MentionPopup — the "Mention · Matching "…"" header driven by the new host-
// set `query` property, the ADMIN/MOD role chip mapped from
// MentionSuggestionModel::RoleRole (real power-level data on the Rust
// backend; the mock's "default" role never shows a chip), the tinted
// return-keycap on the selected row only, the newly-added row accessibility,
// and that @room/presence are never fabricated (the model has no data to
// honestly back either). Drives the REAL MentionSuggestionModel through
// MockMatrixClient's requestRoomMembers snapshot shape — the same "role" key
// both backends already send (see RustSdkMatrixClient.cpp/MockMatrixClient.cpp)
// — plus the real MentionPopup.qml component loaded through the MatrixClient
// QML module. Complements tests/MentionSuggestionModelTest.cpp (model-only)
// and tests/ComposerQmlTest.cpp (composer wiring), neither of which exercises
// the popup's own redesigned presentation.

#include "matrix/MockMatrixClient.h"
#include "models/MentionSuggestionModel.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest/QtTest>

#include <memory>

namespace {

QString read(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

// The text of `function <name>() { ... }` in a QML source, braces matched.
QString extractFunction(const QString &src, const QString &name)
{
    const int at = src.indexOf(QStringLiteral("function %1(").arg(name));
    if (at < 0)
        return {};
    const int open = src.indexOf(QLatin1Char('{'), at);
    if (open < 0)
        return {};
    int depth = 0;
    for (int i = open; i < src.size(); ++i) {
        if (src.at(i) == QLatin1Char('{'))
            ++depth;
        else if (src.at(i) == QLatin1Char('}') && --depth == 0)
            return src.mid(at, i - at + 1);
    }
    return {};
}

// Stand-in for a composer, holding ONLY what a refresh function reads. The
// C++ ranges property is played by `live`: ONE array object that the test
// changes in place, which is how Qt 6.8 hands QML a QVariantList property --
// a reference that re-reads the property on every access.
const char *kRefreshScene = R"QML(
import QtQuick
import MatrixClient

Item {
    id: %1
    property var live: []
    property QtObject app: QtObject {
        property QtObject composer: QtObject { property var mentionRanges: %1.live }
        property QtObject thread: QtObject { property var mentionRanges: %1.live }
    }
    property int mentionTokenStart: -1
    property int threadMentionTokenStart: -1
    property var mentionHighlightRanges: []
    property var threadMentionHighlightRanges: []
    QtObject { id: mentionPopup; property bool visible: false }
    QtObject { id: threadMentionPopup; property bool visible: false }
    QtObject { id: input; property int cursorPosition: 0 }
    QtObject { id: threadComposerInput; property int cursorPosition: 0 }
    MentionHighlighter { objectName: "highlighter"; ranges: %1.%2 }

    function remember() { %1.live = [{ start: 0, length: 16 }] }
    function forget() { %1.live.length = 0 }
    %3
}
)QML";

// Test-driven member snapshot delivery — mirrors
// tests/MentionSuggestionModelTest.cpp's MemberMock, kept self-contained per
// QtTest's one-executable-per-.moc convention.
class MemberMock : public MockMatrixClient
{
    Q_OBJECT
public:
    quint64 requestRoomMembers(const QString &roomId) override
    {
        Q_UNUSED(roomId);
        return ++m_op;
    }
    void deliver(const QString &roomId, const QVariantList &members)
    {
        QVariantMap snapshot;
        snapshot.insert(QStringLiteral("ok"), true);
        snapshot.insert(QStringLiteral("members"), members);
        Q_EMIT roomMembersReceived(m_op, roomId, snapshot);
    }
    quint64 m_op = 0;
};

QVariantMap member(const QString &userId, const QString &name,
                   const QString &role)
{
    QVariantMap m;
    m.insert(QStringLiteral("userId"), userId);
    m.insert(QStringLiteral("displayName"), name);
    m.insert(QStringLiteral("avatarUrl"), QString());
    m.insert(QStringLiteral("membership"), QStringLiteral("join"));
    m.insert(QStringLiteral("role"), role);
    m.insert(QStringLiteral("ambiguous"), false);
    m.insert(QStringLiteral("isOwn"), false);
    return m;
}

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 500
    height: 420
    visible: true

    MentionPopup {
        id: popup
        objectName: "popup"
        suggestions: mentionModel
        anchorInputTop: Qt.point(20, 380)
        anchorWidth: 320
    }
}
)QML";

} // namespace

// Minimal `app` stand-in for the popup's Avatar rows: their media
// Connections resolve app.mediaBridge, and a missing context target would
// fall back to the Connections' parent and warn on every row. Signatures
// mirror MediaBridge exactly; the signals are never emitted here.
class FakeMediaBridge : public QObject
{
    Q_OBJECT

public:
    explicit FakeMediaBridge(QObject *parent = nullptr) : QObject(parent) {}

Q_SIGNALS:
    void mediaCached(const QString &cacheKey);
    void mediaFetchFailed(const QString &cacheKey, const QString &category);
    void mediaRetryable(const QString &cacheKey);
};

class FakeAppContext : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *mediaBridge READ mediaBridge CONSTANT)

public:
    explicit FakeAppContext(QObject *parent = nullptr)
        : QObject(parent), m_bridge(new FakeMediaBridge(this))
    {
    }
    QObject *mediaBridge() const { return m_bridge; }

private:
    FakeMediaBridge *m_bridge;
};

class MentionPopupContractTest : public QObject
{
    Q_OBJECT

private:
    MemberMock m_mock;
    MentionSuggestionModel m_model;

    QQuickItem *rowAt(QObject *root, int index) const
    {
        // ListView-created delegates are model-owned, not QObject-parented,
        // so findChild cannot see them — resolve through itemAtIndex on the
        // (static, findable) list instead.
        auto *list = root->findChild<QQuickItem *>(
            QStringLiteral("mentionListView"));
        if (!list)
            return nullptr;
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(list, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(int, index));
        return item;
    }

private Q_SLOTS:
    void initTestCase()
    {
        m_model.setClient(&m_mock);
        m_model.setRoomId(QStringLiteral("!design:hs"));
        m_mock.deliver(QStringLiteral("!design:hs"), {
            member(QStringLiteral("@alice:hs"), QStringLiteral("Alice"),
                  QStringLiteral("administrator")),
            member(QStringLiteral("@bob:hs"), QStringLiteral("Bob"),
                  QStringLiteral("moderator")),
            member(QStringLiteral("@carol:hs"), QStringLiteral("Carol"),
                  QStringLiteral("user")),
        });
        // Three members, plus @room — offered by default and matching an
        // empty query, exactly as it will be when the popup first opens.
        QCOMPARE(m_model.count(), 4);
        QCOMPARE(m_model.get(0).value(QStringLiteral("userId")).toString(),
                 QStringLiteral("@room"));
    }

    // @room is a real suggestion, not a special case bolted onto the view: it
    // is a row in the same model, it sorts FIRST because it is the broadest
    // thing in the list, and it disappears when the account cannot trigger a
    // whole-room notification.
    void theWholeRoomMentionIsOfferedFirstAndOnlyWhenAllowed()
    {
        m_model.setQuery(QString());
        QCOMPARE(m_model.count(), 4);
        const QVariantMap room = m_model.get(0);
        QCOMPARE(room.value(QStringLiteral("userId")).toString(),
                 QStringLiteral("@room"));
        QVERIFY(room.value(QStringLiteral("isRoom")).toBool());
        // "room", never "@room": the insertion builder prefixes the @ itself,
        // and carrying it here composes "@@room" in the message.
        QCOMPARE(room.value(QStringLiteral("displayName")).toString(),
                 QStringLiteral("room"));

        // Prefixes of "room" keep it; anything else drops it.
        for (const QString &q : { QStringLiteral("r"), QStringLiteral("ro"),
                                  QStringLiteral("room"), QStringLiteral("ROOM") }) {
            m_model.setQuery(q);
            QVERIFY2(m_model.get(0).value(QStringLiteral("isRoom")).toBool(),
                     qPrintable(QStringLiteral("query %1 dropped @room").arg(q)));
        }
        m_model.setQuery(QStringLiteral("ali"));
        QVERIFY(!m_model.get(0).value(QStringLiteral("isRoom")).toBool());

        // Not permitted: gone entirely, in every query that would match it.
        m_model.setRoomMentionAllowed(false);
        m_model.setQuery(QString());
        QCOMPARE(m_model.count(), 3);
        QVERIFY(!m_model.get(0).value(QStringLiteral("isRoom")).toBool());
        m_model.setRoomMentionAllowed(true);
        m_model.setQuery(QString());
    }

    // ── Source-scan: structure, omissions, preserved invariants ──────
    void headerUsesHostSetQueryProperty()
    {
        const QString popup = read(QStringLiteral(QML_DIR "/MentionPopup.qml"));
        QVERIFY(!popup.isEmpty());
        QVERIFY(popup.contains(QStringLiteral("property string query: \"\"")));
        QVERIFY(popup.contains(QStringLiteral(
            "text: qsTr(\"Mention · Matching \\\"%1\\\"\").arg(root.query)")));
        QVERIFY(popup.contains(QStringLiteral("font.family: AppTheme.monoFont")));
        QVERIFY(popup.contains(QStringLiteral("font.capitalization: Font.AllUppercase")));
        // Storm skin: the header rides the faint storm mono ink (deliberate
        // decorative-scale dim, SPEC-storm-language §2) — never a themed ink.
        QVERIFY(popup.contains(QStringLiteral("color: AppTheme.stormTextFaint")));
    }

    void mxidUsesTextMutedPerRuleR4()
    {
        const QString popup = read(QStringLiteral(QML_DIR "/MentionPopup.qml"));
        // Storm skin: MXIDs ride the muted storm mono ink (§2), brightening
        // one step on the selected row for AA on the selection fill.
        QVERIFY(popup.contains(QStringLiteral(
            "? AppTheme.stormTextSecondary")));
        QVERIFY(popup.contains(QStringLiteral(
            ": AppTheme.stormTextMuted")));
    }

    void theRoomRowSaysWhatItDoesAndPresenceStaysOut()
    {
        const QString popup = read(QStringLiteral(QML_DIR "/MentionPopup.qml"));
        // @room was omitted for as long as there was nothing honest behind
        // it. There is now: the offer is gated on the room's OWN required
        // level for a whole-room notification, asked of the SDK. So the row
        // exists, and it says what it does rather than repeating its own
        // name — this is the one suggestion whose consequence is worth
        // spelling out before it is pressed.
        QVERIFY(popup.contains(QStringLiteral("Notify everyone in this room")));
        QVERIFY(popup.contains(QStringLiteral("model.isRoom === true")));

        // Presence stays out, and that is still a deliberate product choice:
        // suggestion rows are transient type-ahead UI, not a roster. If it is
        // ever added it must arrive via the shared PresenceDot, and this
        // assertion pins that the popup never paints its own presence
        // colours.
        QVERIFY(!popup.contains(QStringLiteral("presenceOnline")));
        QVERIFY(!popup.contains(QStringLiteral("presenceAway")));
    }

    void rowsGainedAccessibility()
    {
        const QString popup = read(QStringLiteral(QML_DIR "/MentionPopup.qml"));
        const int delegateStart = popup.indexOf(QStringLiteral("delegate: Rectangle {"));
        QVERIFY(delegateStart >= 0);
        QVERIFY(popup.indexOf(QStringLiteral("Accessible.role: Accessible.Button"),
                              delegateStart) > delegateStart);
        QVERIFY(popup.indexOf(QStringLiteral("Accessible.name:"), delegateStart)
                 > delegateStart);
        QVERIFY(popup.indexOf(QStringLiteral("Accessible.selected: isSelected"),
                              delegateStart) > delegateStart);
    }

    void preservedInvariantsSurviveTheRedesign()
    {
        const QString popup = read(QStringLiteral(QML_DIR "/MentionPopup.qml"));
        QVERIFY(popup.contains(QStringLiteral("focus: false")));
        QVERIFY(popup.contains(QStringLiteral("closePolicy: Popup.NoAutoClose")));
        QVERIFY(popup.contains(
            QStringLiteral("currentIndex = (currentIndex + 1) % count")));
        QVERIFY(popup.contains(
            QStringLiteral("currentIndex = (currentIndex - 1 + count) % count")));
        QVERIFY(popup.contains(QStringLiteral("if (!m || !m.userId)")));
        QVERIFY(popup.contains(QStringLiteral("if (visible && count === 0)")));
        QVERIFY(popup.contains(QStringLiteral("close()")));
        // Members-only sourcing: never a directory/server search call.
        QVERIFY(!popup.contains(QStringLiteral("searchDirectory")));
        QVERIFY(!popup.contains(QStringLiteral("userDirectorySearch")));
    }

    // ── Behavior: role chip mapping, header text, selected keycap ───
    void chipHeaderAndKeycapRenderFromLiveState()
    {
        QQmlApplicationEngine engine;
        QStringList warnings;
        connect(&engine, &QQmlEngine::warnings, this,
                [&warnings](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        warnings << e.toString();
                });
        FakeAppContext appContext;
        engine.rootContext()->setContextProperty("app", &appContext);
        engine.rootContext()->setContextProperty("mentionModel", &m_model);
        QQmlComponent component(&engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("mentionpopupscene.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));
        QCoreApplication::processEvents();

        auto *popup = root->findChild<QObject *>(QStringLiteral("popup"));
        QVERIFY(popup != nullptr);
        popup->setProperty("query", QStringLiteral("al"));
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        // Wait for the FULLY opened state (not just "visible"), so
        // onOpened's currentIndex = 0 reset has already settled before the
        // test drives currentIndex itself below.
        QTRY_VERIFY(popup->property("opened").toBool());

        // The popup NEVER fabricates a row: it shows exactly what the model
        // holds. With the whole-room mention turned off that is the three
        // members and nothing else — the @room row is a model row like any
        // other, covered by its own case above.
        m_model.setRoomMentionAllowed(false);
        QTRY_COMPARE(popup->property("count").toInt(), 3);

        auto *header =
            root->findChild<QQuickItem *>(QStringLiteral("mentionPopupHeader"));
        QVERIFY(header != nullptr);
        QCOMPARE(header->property("text").toString(),
                 QStringLiteral("Mention · Matching \"al\""));

        // Row 0 = Alice (administrator) -> ADMIN chip. Delegate creation
        // lands on the ListView's next layout pass, not synchronously with
        // `opened` — wait for the first row to materialize.
        QTRY_VERIFY(rowAt(root, 0) != nullptr);
        auto *row0 = rowAt(root, 0);
        auto *chip0 = row0->findChild<QQuickItem *>(QStringLiteral("mentionRoleChip"));
        QVERIFY(chip0 != nullptr);
        QVERIFY(chip0->property("visible").toBool());
        QCOMPARE(chip0->property("label").toString(), QStringLiteral("ADMIN"));

        // Row 1 = Bob (moderator) -> MOD chip.
        auto *row1 = rowAt(root, 1);
        QVERIFY(row1 != nullptr);
        auto *chip1 = row1->findChild<QQuickItem *>(QStringLiteral("mentionRoleChip"));
        QVERIFY(chip1 != nullptr);
        QVERIFY(chip1->property("visible").toBool());
        QCOMPARE(chip1->property("label").toString(), QStringLiteral("MOD"));

        // Row 2 = Carol (plain "user") -> no chip.
        auto *row2 = rowAt(root, 2);
        QVERIFY(row2 != nullptr);
        auto *chip2 = row2->findChild<QQuickItem *>(QStringLiteral("mentionRoleChip"));
        QVERIFY(chip2 != nullptr);
        QVERIFY(!chip2->property("visible").toBool());

        // Selected-row keycap: only the current row shows it.
        popup->setProperty("currentIndex", 1);
        auto *keycap0 = row0->findChild<QQuickItem *>(QStringLiteral("mentionSelectedKeycap"));
        auto *keycap1 = row1->findChild<QQuickItem *>(QStringLiteral("mentionSelectedKeycap"));
        QVERIFY(keycap0 != nullptr && keycap1 != nullptr);
        QTRY_VERIFY(keycap1->property("visible").toBool());
        QVERIFY(!keycap0->property("visible").toBool());

        // accept() resolves the currently-selected row's real userId.
        QSignalSpy chosen(popup, SIGNAL(chosen(QString, QString)));
        QVERIFY(QMetaObject::invokeMethod(popup, "accept"));
        QCOMPARE(chosen.count(), 1);
        QCOMPARE(chosen.at(0).at(0).toString(), QStringLiteral("@bob:hs"));

        delete root;
        QCOMPARE(warnings, QStringList{});
    }

    void moveDownAndMoveUpWrapAcrossEnds()
    {
        QQmlApplicationEngine engine;
        FakeAppContext appContext;
        engine.rootContext()->setContextProperty("app", &appContext);
        engine.rootContext()->setContextProperty("mentionModel", &m_model);
        QQmlComponent component(&engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("mentionpopupscene2.qml")));
        QObject *root = component.create();
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root);
        QVERIFY(window != nullptr);
        QVERIFY(QTest::qWaitForWindowExposed(window));

        auto *popup = root->findChild<QObject *>(QStringLiteral("popup"));
        QVERIFY(popup != nullptr);
        QVERIFY(QMetaObject::invokeMethod(popup, "open"));
        // Wait for the FULLY opened state (not just "visible"), so
        // onOpened's currentIndex = 0 reset has already settled before the
        // test drives currentIndex itself below.
        QTRY_VERIFY(popup->property("opened").toBool());

        // Wrap-around is about the ENDS of the list, so pin the length:
        // three members, no whole-room row.
        m_model.setRoomMentionAllowed(false);
        QTRY_COMPARE(popup->property("count").toInt(), 3);

        popup->setProperty("currentIndex", 2);
        QVERIFY(QMetaObject::invokeMethod(popup, "moveDown"));
        QCOMPARE(popup->property("currentIndex").toInt(), 0); // wraps forward

        QVERIFY(QMetaObject::invokeMethod(popup, "moveUp"));
        QCOMPARE(popup->property("currentIndex").toInt(), 2); // wraps backward

        delete root;
    }

    // Reported against the 0.9.9 AppImage: after sending a message with a
    // mention, every NEW message began with an accent-inked run exactly as
    // long as that mention. It was visual only (the next event went out as a
    // plain body), and it came from the refresh functions: they stored the
    // C++ ranges list AS READ, and on Qt 6.8 that is a live reference, so the
    // stored copy always equalled the new value, the "nothing changed" check
    // returned early, and the highlighter never received the empty list. Qt
    // 6.11 detaches the list on store, so the real composer cannot show it in
    // a local build; each composer's OWN refresh function is run here against
    // a ranges list that behaves the way Qt 6.8's does.
    void aForgottenMentionStopsInkingTheComposer_data()
    {
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("function");
        QTest::addColumn<QString>("rootId");
        QTest::addColumn<QString>("mirror");
        QTest::newRow("room composer")
            << QStringLiteral("MessageComposerBar.qml")
            << QStringLiteral("refreshMentionHighlight")
            << QStringLiteral("root")
            << QStringLiteral("mentionHighlightRanges");
        QTest::newRow("thread composer")
            << QStringLiteral("ThreadPanel.qml")
            << QStringLiteral("refreshThreadMentionHighlight")
            << QStringLiteral("panel")
            << QStringLiteral("threadMentionHighlightRanges");
    }

    void aForgottenMentionStopsInkingTheComposer()
    {
        QFETCH(QString, file);
        QFETCH(QString, function);
        QFETCH(QString, rootId);
        QFETCH(QString, mirror);
        const QString body = extractFunction(
            read(QStringLiteral(QML_DIR "/") + file), function);
        QVERIFY2(!body.isEmpty(),
                 qPrintable(QStringLiteral("%1 has no %2()").arg(file, function)));

        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(QString::fromLatin1(kRefreshScene)
                              .arg(rootId, mirror, body)
                              .toUtf8(),
                          QUrl(QStringLiteral("refreshscene.qml")));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *highlighter =
            root->findChild<QObject *>(QStringLiteral("highlighter"));
        QVERIFY(highlighter != nullptr);
        const QByteArray fn = function.toLatin1();
        const auto inked = [&] {
            return highlighter->property("ranges").toList().size();
        };

        // A mention was inserted: the highlighter inks it.
        QVERIFY(QMetaObject::invokeMethod(root.get(), "remember"));
        QVERIFY(QMetaObject::invokeMethod(root.get(), fn.constData()));
        QCOMPARE(inked(), 1);

        // The message was sent and the composer forgot the mention. The
        // highlighter must forget it too, or the next message is inked.
        QVERIFY(QMetaObject::invokeMethod(root.get(), "forget"));
        QVERIFY(QMetaObject::invokeMethod(root.get(), fn.constData()));
        QCOMPARE(inked(), 0);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    MentionPopupContractTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "MentionPopupContractTest.moc"
