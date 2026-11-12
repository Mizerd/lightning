// MemberProfilePopover: the Matrix ID's double-click copy, with a real mouse.
// Selection goes through the shared
// UserPicker's userSelected signal (as the production handler is wired),
// bypassing search debounce and network, against a real
// AppController(MockBackend). The invited ids are absent from the mock
// room's member map, so the already-member pre-check never engages.

#include <QtTest/QtTest>

#include <QClipboard>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlProperty>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QScopeGuard>
#include <QSettings>
#include <QTemporaryDir>

#include "app/AppController.h"
#include "app/ConversationController.h"
#include "auth/AuthManager.h"
#include "matrix/MockMatrixClient.h"
#include "profile/ProfileBioManager.h"

namespace {

constexpr int kSignalTimeoutMs = 5000;

class FakeBioClient : public MockMatrixClient
{
public:
    bool supportsProfileBios() const override { return true; }
    void fetchProfileBio(const QString &userId, quint64 opId) override
    {
        lastUser = userId;
        lastOp = opId;
    }
    QString lastUser;
    quint64 lastOp = 0;
};

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

    MemberProfilePopover {
        objectName: "profile"
        parent: Overlay.overlay
    }
}
)QML";

}

class MemberProfileCopyQmlTest : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_configHome;
    AppController *m_controller = nullptr;
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

    QQuickItem *item(const char *name) const
    {
        if (auto *hit = m_window->findChild<QQuickItem *>(QLatin1String(name)))
            return hit;
        return findItem(m_window->contentItem(), QLatin1String(name));
    }

    void selectUser(const QString &userId, const QString &displayName)
    {
        auto *picker = m_window->findChild<QObject *>(
            QStringLiteral("invitePeoplePicker"));
        QVERIFY(picker);
        QVERIFY(QMetaObject::invokeMethod(
            picker, "userSelected",
            Q_ARG(QString, userId),
            Q_ARG(QString, displayName),
            Q_ARG(QString, QString())));
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
            QStringLiteral("member-profile-copy-qml-test"));
        QSettings().clear();
        m_controller = new AppController(AppController::MockBackend);
        m_engine = new QQmlEngine(this);
        m_engine->rootContext()->setContextProperty(QStringLiteral("app"),
                                                    m_controller);
        QSignalSpy loginSpy(m_controller->auth(), &AuthManager::loginSucceeded);
        m_controller->auth()->login(QStringLiteral("https://mock.local"),
                                    QStringLiteral("alice"),
                                    QStringLiteral("mock-password-fixture"));
        QVERIFY(loginSpy.wait(kSignalTimeoutMs));
        QTRY_VERIFY(m_controller->loggedIn());
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("scene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));
        m_dialog = m_root->findChild<QObject *>(QStringLiteral("profile"));
        QVERIFY(m_dialog);
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_controller;
    }

    void copyUserIdWritesTheClipboard()
    {
        QVariantMap member;
        member.insert(QStringLiteral("userId"), QStringLiteral("@bob:mock.local"));
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "openFor",
                                          Q_ARG(QVariant, QVariant(member)),
                                          Q_ARG(QVariant, QVariant())));
        QGuiApplication::clipboard()->setText(QStringLiteral("untouched"));
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "copyUserId"));
        QCOMPARE(QGuiApplication::clipboard()->text(),
                 QStringLiteral("@bob:mock.local"));
    }

    void theBannerIsNotLayeredWhereNoEffectCanBeDrawn()
    {
        QVariantMap member;
        member.insert(QStringLiteral("userId"), QStringLiteral("@bob:mock.local"));
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "openFor",
                                          Q_ARG(QVariant, QVariant(member)),
                                          Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        auto *banner = item("profileBanner");
        auto *clip = item("profileBannerClip");
        QVERIFY(banner && clip);
        const bool software = m_window->rendererInterface()->graphicsApi()
                              == QSGRendererInterface::Software;
        QCOMPARE(banner->property("maskable").toBool(), !software);
        QCOMPARE(QQmlProperty(clip, QStringLiteral("layer.enabled"))
                     .read().toBool(), !software);
    }

    void doubleClickOnTheMatrixIdCopiesIt()
    {
        QVariantMap member;
        member.insert(QStringLiteral("userId"), QStringLiteral("@bob:mock.local"));
        member.insert(QStringLiteral("displayName"), QStringLiteral("Bob"));
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "openFor",
                                          Q_ARG(QVariant, QVariant(member)),
                                          Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(m_dialog->property("visible").toBool());
        QGuiApplication::clipboard()->setText(QStringLiteral("untouched"));

        auto *id = item("profileUserId");
        QVERIFY(id);
        QTRY_VERIFY(id->isVisible() && id->width() > 0);
        const QPoint p = id->mapToScene(QPointF(id->width() / 2, id->height() / 2))
                             .toPoint();
        QTest::mouseDClick(m_window, Qt::LeftButton, Qt::NoModifier, p);
        QTRY_COMPARE(QGuiApplication::clipboard()->text(),
                     QStringLiteral("@bob:mock.local"));
        auto *notice = item("profileCopiedNotice");
        QVERIFY(notice);
        QVERIFY(notice->isVisible());
        QCOMPARE(notice->property("text").toString(),
                 QStringLiteral("Matrix ID copied"));
    }
    void aLongBioScrollsInsideACappedBoxAndKeepsTheActionsOnTheCard()
    {
        FakeBioClient bioClient;
        m_controller->bio()->setClient(&bioClient);
        const auto restore = qScopeGuard([this] {
            m_controller->bio()->setClient(nullptr);
        });

        QVERIFY(QMetaObject::invokeMethod(m_dialog, "close"));
        QTRY_VERIFY(!m_dialog->property("visible").toBool());
        const QString user = QStringLiteral("@longbio:mock.local");
        QVariantMap member;
        member.insert(QStringLiteral("userId"), user);
        QVERIFY(QMetaObject::invokeMethod(m_dialog, "openFor",
                                          Q_ARG(QVariant, QVariant(member)),
                                          Q_ARG(QVariant, QVariant())));
        QTRY_VERIFY(m_dialog->property("opened").toBool());
        QTRY_COMPARE(bioClient.lastUser, user);
        QStringList paragraphs;
        for (int i = 0; i < 40; ++i)
            paragraphs << QStringLiteral("Paragraph %1 of a long biography, long "
                                         "enough to wrap onto a second line.").arg(i);
        Q_EMIT bioClient.profileBioReceived(bioClient.lastOp, user,
                                            paragraphs.join(QLatin1Char('\n')),
                                            true);

        auto *box = item("profileBioBox");
        auto *flick = item("profileBioFlick");
        auto *text = item("profileBioText");
        auto *bar = item("profileBioScrollBar");
        auto *actions = item("profileActionRow");
        QVERIFY(box && flick && text && bar && actions);
        QTRY_VERIFY(text->implicitHeight() > 2 * box->height());

        auto *card = m_dialog->property("contentItem").value<QQuickItem *>();
        QVERIFY(card);
        QTRY_VERIFY(m_dialog->property("y").toReal()
                        + m_dialog->property("height").toReal()
                    <= m_window->height());
        const auto rectIn = [card](QQuickItem *child) {
            return child->mapRectToItem(card, child->boundingRect());
        };
        const auto inside = [card, rectIn](QQuickItem *child) {
            const QRectF r = rectIn(child);
            return r.top() >= 0 && r.bottom() <= card->height() + 0.5
                && r.left() >= 0 && r.right() <= card->width() + 0.5;
        };
        const auto describe = [card, rectIn](QQuickItem *child) {
            const QRectF r = rectIn(child);
            return QStringLiteral("%1,%2 %3x%4 in card %5x%6")
                .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())
                .arg(card->width()).arg(card->height());
        };
        // The layout settles in a later polish pass than the bio's arrival.
        QTRY_VERIFY2(inside(box), qPrintable(describe(box)));
        QTRY_VERIFY2(inside(actions), qPrintable(describe(actions)));
        QVERIFY(actions->height() > 0);
        const QRectF fr = flick->mapRectToItem(box, flick->boundingRect());
        QVERIFY(fr.top() >= 0 && fr.bottom() <= box->height());

        QVERIFY(flick->property("contentHeight").toReal()
                > flick->height() * 2);
        QVERIFY(bar->isVisible());
        QCOMPARE(flick->property("contentY").toReal(), 0.0);

        const QPointF local(flick->width() / 2, flick->height() / 2);
        const QPointF scenePos = flick->mapToScene(local);
        QWheelEvent wheel(scenePos, m_window->mapToGlobal(scenePos), QPoint(),
                          QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(m_window, &wheel);
        QTRY_VERIFY(flick->property("contentY").toReal() > 0);
        QVERIFY(m_dialog->property("visible").toBool());

        text->forceActiveFocus();
        QTRY_VERIFY(text->hasActiveFocus());
        const qreal beforePage = flick->property("contentY").toReal();
        QTest::keyClick(m_window, Qt::Key_PageDown);
        QTRY_VERIFY(flick->property("contentY").toReal() > beforePage);
        QTest::keyClick(m_window, Qt::Key_End);
        QTRY_COMPARE(flick->property("contentY").toReal(),
                     flick->property("contentHeight").toReal() - flick->height());
        QVERIFY(m_dialog->property("visible").toBool());
    }
};

QTEST_MAIN(MemberProfileCopyQmlTest)
#include "MemberProfileCopyQmlTest.moc"
