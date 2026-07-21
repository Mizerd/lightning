// Creation UX proof for the redesigned NewConversationDialog: the Space tab
// instantiates NO encryption control while the Room tab does (tabs are
// Loaders, not visible-toggled columns); a tab switch clears the SHARED
// UserSearchModel and hands focus to the new tab's first field; Create stays
// disabled until the name is non-blank and the single error banner surfaces
// the controller's errorMessage; the controller's busy state disables submit
// while Cancel stays enabled; existing-DM reuse rows render BEFORE the
// clearly-secondary "start anyway" action; and Down/Down/Return in the
// search field selects the second result.
//
// Pattern note: this uses the focused-scene approach (like ComposerQmlTest)
// rather than the full Main.qml shell (SettingsShellQmlTest), because the
// dialog lives on Overlay.overlay and needs a deterministic FakeClient
// injected into app.conversations — busy/search/existing-DM state the
// full-shell mock backend cannot script.

#include <QtTest/QtTest>

#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/ConversationController.h"
#include "matrix/MatrixClient.h"
#include "models/UserSearchModel.h"

namespace {

class FakeClient final : public MatrixClient
{
    Q_OBJECT
public:
    using MatrixClient::MatrixClient;

    // Scripted state.
    QList<RoomInfo> mirror;
    QVariantList dmRooms;
    quint64 nextOp = 1;
    int searchCalls = 0;
    int createRoomCalls = 0;
    int createDmCalls = 0;
    QString lastSearchQuery;
    QVariantMap lastRoomOptions;
    quint64 lastOpId = 0;

    // MatrixClient pure virtuals (inert).
    void login(const QString &, const QString &, const QString &) override {}
    void logout() override { Q_EMIT loggedOut(); }
    bool restoreSession() override { return false; }
    bool isLoggedIn() const override { return true; }
    QString currentUserId() const override { return QStringLiteral("@me:example.org"); }
    QString homeserverUrl() const override { return {}; }
    void startSync() override {}
    void stopSync() override {}
    ConnectionState connectionState() const override { return Syncing; }
    QList<RoomInfo> rooms() const override { return mirror; }
    QList<TimelineEvent> timeline(const QString &) const override { return {}; }
    QString displayNameFor(const QString &, const QString &id) const override { return id; }
    QString avatarMxcFor(const QString &, const QString &) const override { return {}; }
    QStringList typingUsersFor(const QString &) const override { return {}; }
    QUrl mediaDownloadUrl(const QString &) const override { return {}; }
    QUrl mediaThumbnailUrl(const QString &, int, int, bool) const override { return {}; }
    void sendTextMessage(const QString &, const QString &) override {}
    void sendReply(const QString &, const QString &, const QString &) override {}
    void editMessage(const QString &, const QString &, const QString &) override {}
    void redactEvent(const QString &, const QString &, const QString &) override {}
    void toggleReaction(const QString &, const QString &, const QString &) override {}
    void sendTyping(const QString &, bool, int) override {}
    void sendReadReceipt(const QString &, const QString &) override {}
    void sendImage(const QString &, const QString &) override {}
    void sendFile(const QString &, const QString &) override {}
    void loadOlderMessages(const QString &) override {}
    bool canPaginate(const QString &) const override { return false; }
    bool paginating(const QString &) const override { return false; }

    bool supportsRoomManagement() const override { return true; }
    quint64 searchUsers(const QString &query, int) override
    {
        ++searchCalls;
        lastSearchQuery = query;
        lastOpId = nextOp++;
        return lastOpId;
    }
    QVariantList existingDirectRooms(const QString &) const override
    {
        return dmRooms;
    }
    quint64 createDirectChat(const QString &) override
    {
        ++createDmCalls;
        lastOpId = nextOp++;
        return lastOpId;
    }
    quint64 createRoom(const QVariantMap &options) override
    {
        ++createRoomCalls;
        lastRoomOptions = options;
        lastOpId = nextOp++;
        return lastOpId;
    }
};

QVariantMap searchRow(const QString &userId, const QString &name = {})
{
    QVariantMap row;
    row.insert(QStringLiteral("userId"), userId);
    row.insert(QStringLiteral("displayName"), name);
    row.insert(QStringLiteral("avatarUrl"), QString());
    return row;
}

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 900
    height: 700
    visible: true
    color: AppTheme.background

    NewConversationDialog {
        objectName: "creationDialog"
        parent: Overlay.overlay
    }
}
)QML";

} // namespace

class CreationDialogQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
    FakeClient *m_fake = nullptr;
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;
    QObject *m_dialog = nullptr;
    QStringList m_warnings;

    static QQuickItem *findItem(QQuickItem *parent, const QString &name)
    {
        if (!parent)
            return nullptr;
        if (parent->objectName() == name)
            return parent;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            if (QQuickItem *hit = findItem(child, name))
                return hit;
        }
        return nullptr;
    }

    // Popup contents live in the Overlay's visual tree, not the QObject
    // parent chain, so findChild alone cannot see them.
    QQuickItem *item(const char *name) const
    {
        if (auto *hit = m_window->findChild<QQuickItem *>(QLatin1String(name)))
            return hit;
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    static void collectItems(QQuickItem *parent, QList<QQuickItem *> *out)
    {
        if (!parent)
            return;
        const auto children = parent->childItems();
        for (QQuickItem *child : children) {
            out->append(child);
            collectItems(child, out);
        }
    }

    static bool subtreeHasClassName(QQuickItem *rootItem, const char *fragment)
    {
        QList<QQuickItem *> all;
        collectItems(rootItem, &all);
        for (QQuickItem *it : all) {
            if (QByteArray(it->metaObject()->className()).contains(fragment))
                return true;
        }
        return false;
    }

    void openDialog(const char *mode)
    {
        QVERIFY(QMetaObject::invokeMethod(
            m_dialog, "openDialog",
            Q_ARG(QVariant, QVariant(QLatin1String(mode)))));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        QCoreApplication::processEvents();
    }

    void closeDialog()
    {
        QMetaObject::invokeMethod(m_dialog, "close");
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        QCoreApplication::processEvents();
    }

    ConversationController *conversations() const
    {
        return m_controller->conversations();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_configHome.isValid());
        qputenv("XDG_CONFIG_HOME", m_configHome.path().toUtf8());
        QCoreApplication::setOrganizationName(
            QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("creation-dialog-qml-test"));
        QSettings().clear();

        m_controller = new AppController(AppController::MockBackend);
        m_fake = new FakeClient;
        m_engine = new QQmlEngine(this);
        connect(m_engine, &QQmlEngine::warnings, this,
                [this](const QList<QQmlError> &warnings) {
                    for (const auto &w : warnings)
                        m_warnings.append(w.toString());
                });
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene),
                          QUrl(QStringLiteral("creationscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        QCoreApplication::processEvents();

        m_dialog = m_root->findChild<QObject *>(QStringLiteral("creationDialog"));
        QVERIFY(m_dialog);

        // Deterministic backend: script busy/search/existing-DM behavior.
        conversations()->setClient(m_fake);
        conversations()->userSearch()->setDebounceMs(1);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
        delete m_fake;
    }

    void loadsWithoutQmlWarnings()
    {
        QCOMPARE(m_warnings, QStringList{});
    }

    // (a) The Space tab instantiates NO encryption control; the Room tab
    // does. Loaders guarantee non-instantiation, not mere invisibility.
    void spaceTabInstantiatesNoEncryptionControl()
    {
        openDialog("space");
        QTRY_VERIFY(item("spaceTab"));
        QVERIFY(item("spaceNameField"));
        QVERIFY(item("spaceCreateButton"));
        QVERIFY(!item("roomTab"));
        QVERIFY(!item("roomEncryptSwitch"));
        QVERIFY(!item("roomEncryptRow"));
        // No switch of any kind inside the Space tab's subtree.
        QVERIFY(!subtreeHasClassName(item("spaceTab"), "AppSwitch"));

        auto *roomSegment = item("creationModeTabs_room");
        QVERIFY(roomSegment);
        QMetaObject::invokeMethod(roomSegment, "click");
        QTRY_VERIFY(item("roomTab"));
        // Loader teardown releases the old tab through deferred deletion —
        // let the event loop run it before asserting non-existence.
        QTRY_VERIFY(!item("spaceTab"));
        QVERIFY(item("roomEncryptSwitch"));
        QVERIFY(subtreeHasClassName(item("roomTab"), "AppSwitch"));
        closeDialog();
    }

    // (b) A tab switch clears the shared search model and moves focus to
    // the new tab's first field.
    void tabSwitchClearsSearchAndMovesFocus()
    {
        openDialog("dm");
        auto *field = item("dmUserPickerSearchField");
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());

        const int before = m_fake->searchCalls;
        field->setProperty("text", QStringLiteral("ali"));
        QTRY_VERIFY(m_fake->searchCalls > before);
        QCOMPARE(conversations()->userSearch()->query(), QStringLiteral("ali"));
        QCOMPARE(conversations()->userSearch()->state(),
                 QStringLiteral("loading"));

        auto *roomSegment = item("creationModeTabs_room");
        QVERIFY(roomSegment);
        QMetaObject::invokeMethod(roomSegment, "click");
        QTRY_VERIFY(item("roomNameField"));
        // Shared model cleared…
        QCOMPARE(conversations()->userSearch()->query(), QString());
        QCOMPARE(conversations()->userSearch()->state(), QStringLiteral("idle"));
        // …and focus handed to the Room tab's first field.
        QTRY_VERIFY(item("roomNameField")->hasActiveFocus());
        closeDialog();
    }

    // (c) Create stays disabled until the name is non-blank; the blank-after-
    // edit hint appears; the single banner surfaces controller errors.
    void createDisabledUntilNameAndErrorBannerShows()
    {
        openDialog("room");
        auto *nameField = item("roomNameField");
        auto *create = item("roomCreateButton");
        QVERIFY(nameField && create);
        QVERIFY(!create->property("enabled").toBool());
        QVERIFY(!item("roomNameHint")->isVisible());

        QTRY_VERIFY(nameField->hasActiveFocus());
        QTest::keyClick(m_window, Qt::Key_A);
        QTRY_COMPARE(nameField->property("text").toString(), QStringLiteral("a"));
        QVERIFY(create->property("enabled").toBool());

        QTest::keyClick(m_window, Qt::Key_Backspace);
        QTRY_COMPARE(nameField->property("text").toString(), QString());
        QVERIFY(!create->property("enabled").toBool());
        // Blank after an edit → inline validation hint.
        QTRY_VERIFY(item("roomNameHint")->isVisible());

        // The single error banner mirrors the controller's errorMessage.
        auto *banner = item("creationErrorBanner");
        QVERIFY(banner);
        QVERIFY(!banner->isVisible());
        conversations()->createRoom({ { QStringLiteral("name"), QString() } });
        QTRY_VERIFY(banner->isVisible());
        QVERIFY(!conversations()->errorMessage().isEmpty());
        conversations()->clearError();
        QTRY_VERIFY(!banner->isVisible());
        closeDialog();
    }

    // (d) Busy disables submit; Cancel stays enabled; a failure surfaces in
    // the banner and ends the busy state.
    void busyDisablesSubmitButNeverCancel()
    {
        openDialog("room");
        auto *nameField = item("roomNameField");
        auto *create = item("roomCreateButton");
        auto *cancel = item("creationCancelButton");
        QVERIFY(nameField && create && cancel);

        nameField->setProperty("text", QStringLiteral("Test room"));
        QTRY_VERIFY(create->property("enabled").toBool());
        const int before = m_fake->createRoomCalls;
        QMetaObject::invokeMethod(create, "click");
        QCOMPARE(m_fake->createRoomCalls, before + 1);
        // Private default submits encrypted && !public; the optional avatar
        // never reaches the backend's create call.
        QCOMPARE(m_fake->lastRoomOptions.value(QStringLiteral("name")).toString(),
                 QStringLiteral("Test room"));
        QCOMPARE(m_fake->lastRoomOptions.value(QStringLiteral("public")).toBool(),
                 false);
        QCOMPARE(m_fake->lastRoomOptions.value(QStringLiteral("encrypted")).toBool(),
                 true);
        QVERIFY(!m_fake->lastRoomOptions.contains(QStringLiteral("avatarPath")));

        QVERIFY(conversations()->busy());
        QTRY_VERIFY(!create->property("enabled").toBool());
        QVERIFY(item("creationBusyLabel")->isVisible());
        QVERIFY(cancel->property("enabled").toBool());

        Q_EMIT m_fake->roomCreateFinished(m_fake->lastOpId, false, QString(),
                                          QStringLiteral("network"), QString());
        QTRY_VERIFY(!conversations()->busy());
        QTRY_VERIFY(item("creationErrorBanner")->isVisible());
        QTRY_VERIFY(create->property("enabled").toBool());
        closeDialog();
    }

    // (e)+(f) Down/Down/Return selects the SECOND search result; with an
    // existing DM the reuse row renders BEFORE the clearly-secondary
    // "start anyway" action and the primary start button is not shown.
    void keyboardSelectsSecondResultAndReuseComesFirst()
    {
        QVariantMap dm;
        dm.insert(QStringLiteral("roomId"), QStringLiteral("!dm:example.org"));
        dm.insert(QStringLiteral("name"), QStringLiteral("Alice Two"));
        m_fake->dmRooms = { dm };

        openDialog("dm");
        auto *field = item("dmUserPickerSearchField");
        QVERIFY(field);
        QTRY_VERIFY(field->hasActiveFocus());

        const int before = m_fake->searchCalls;
        field->setProperty("text", QStringLiteral("alice"));
        QTRY_VERIFY(m_fake->searchCalls > before);
        Q_EMIT m_fake->userSearchFinished(
            m_fake->lastOpId, true,
            { searchRow(QStringLiteral("@alice:example.org"),
                        QStringLiteral("Alice")),
              searchRow(QStringLiteral("@alice2:example.org"),
                        QStringLiteral("Alice Two")) },
            false, QString());
        auto *results = item("dmUserPickerResults");
        QVERIFY(results);
        QTRY_COMPARE(results->property("count").toInt(), 2);

        QTest::keyClick(m_window, Qt::Key_Down);
        QTest::keyClick(m_window, Qt::Key_Down);
        QTest::keyClick(m_window, Qt::Key_Return);
        QTRY_COMPARE(m_dialog->property("selectedUserId").toString(),
                     QStringLiteral("@alice2:example.org"));
        QCOMPARE(m_dialog->property("selectedDisplayName").toString(),
                 QStringLiteral("Alice Two"));

        // Existing-DM reuse first, secondary escape hatch below, no primary.
        QCOMPARE(conversations()->existingDms().size(), 1);
        auto *reuseRow = item("existingDmRow_0");
        auto *openBtn = item("existingDmOpen_0");
        auto *anyway = item("dmStartAnywayButton");
        QVERIFY(reuseRow && reuseRow->isVisible());
        QVERIFY(openBtn && openBtn->isVisible());
        QVERIFY(anyway && anyway->isVisible());
        QCOMPARE(anyway->property("kind").toString(),
                 QStringLiteral("secondary"));
        auto *primary = item("dmStartButton");
        QVERIFY(!primary || !primary->isVisible());
        // Ordering settles with layout polish; poll rather than assert the
        // very first frame.
        QTRY_VERIFY(reuseRow->mapToScene(QPointF(0, 0)).y()
                    < anyway->mapToScene(QPointF(0, 0)).y());

        m_fake->dmRooms.clear();
        closeDialog();
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    CreationDialogQmlTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "CreationDialogQmlTest.moc"
