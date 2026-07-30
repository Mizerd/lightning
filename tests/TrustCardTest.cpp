// v0.6.5 (SPEC 1r): offscreen proof for the standalone TrustCard component.
// TrustCard never touches `app.*` (the embedding surface owns all real
// bindings), so this test drives it purely through property injection:
// brand tokens (never the active theme palette), complete vs pending node
// rendering, the both-ends-complete connector rule, the Verify-only action
// (never Message, never QR), and that no theme mode change perturbs the
// brand-fixed navy/yellow palette.

#include <QtTest/QtTest>

#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>

#include <cmath>
#include <QRegularExpression>
#include <QSignalSpy>

namespace {

const char *kScene = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 420
    height: 420
    visible: true
    color: AppTheme.background

    property int themeMode: 9
    Binding { target: AppTheme; property: "mode"; value: win.themeMode }

    Rectangle { objectName: "tokTrustYellow"; visible: false; color: AppTheme.trustYellow }
    Rectangle { objectName: "tokTrustNavy"; visible: false; color: AppTheme.trustNavy }
    Rectangle { objectName: "tokTrustPending"; visible: false; color: AppTheme.trustPending }

    TrustCard {
        id: card
        objectName: "card"
        x: 20; y: 20
        displayName: "Alice Example"
        userId: "@alice:example.org"
        steps: [
            { label: "IDENTITY", iconName: "person", complete: true },
            { label: "DEVICES", iconName: "devices", complete: true },
            { label: "CROSS-SIGN", iconName: "key", complete: false }
        ]
        statusText: "2 of 3 checks complete"
        showVerify: true
    }

    signal verifySignal()
    Connections {
        target: card
        function onVerifyRequested() { win.verifySignal() }
    }
}
)QML";

} // namespace

class TrustCardTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine *m_engine = nullptr;
    QObject *m_root = nullptr;
    QQuickWindow *m_window = nullptr;

    QObject *find(const QString &name) const
    {
        return m_root->findChild<QObject *>(name);
    }
    QColor token(const QString &name) const
    {
        auto *item = find(name);
        return item ? item->property("color").value<QColor>() : QColor();
    }

private slots:
    void initTestCase()
    {
        // The bundled faces main.cpp loads — without them the brand face
        // falls back and every Icon glyph renders tofu (the assertions pass
        // either way, but the optional snapshot must show production type).
        for (const char *font :
             { "MaterialSymbolsRounded-subset.ttf", "SpaceGrotesk[wght].ttf",
               "Manrope[wght].ttf", "JetBrainsMono[wght].ttf" }) {
            QFontDatabase::addApplicationFont(
                QStringLiteral(":/qt/qml/MatrixClient/data/fonts/")
                + QLatin1String(font));
        }

        m_engine = new QQmlEngine;
        QQmlComponent component(m_engine);
        component.setData(QByteArray(kScene), QUrl(QStringLiteral("trustcardscene.qml")));
        m_root = component.create();
        QVERIFY2(m_root, qPrintable(component.errorString()));
        component.setParent(m_root);
        m_window = qobject_cast<QQuickWindow *>(m_root);
        QVERIFY(m_window);
        QVERIFY(QTest::qWaitForWindowExposed(m_window));

        // Optional visual-comparison artifact: the mock backend cannot
        // render the card in the demo app (crypto unsupported), so this
        // fictional-data scene doubles as the reference capture when
        // LIGHTNING_TRUSTCARD_SNAPSHOT names an output path. Test behavior
        // is unchanged when the variable is unset.
        const QString snapshotPath = qEnvironmentVariable(
            "LIGHTNING_TRUSTCARD_SNAPSHOT");
        if (!snapshotPath.isEmpty()) {
            const QImage shot = m_window->grabWindow();
            QVERIFY(!shot.isNull());
            QVERIFY(shot.save(snapshotPath));
        }
    }

    void cleanupTestCase()
    {
        delete m_root;
        delete m_engine;
    }

    void noQrWordingInAnyUserFacingString()
    {
        // The banned icon name, and no qsTr() string mentioning QR/scanning
        // (QR verification does not exist — SAS is the only real flow).
        // Explanatory source comments documenting that ruling are not
        // user-facing wording, so this checks qsTr() call sites specifically
        // rather than banning the substring across the whole file.
        QFile file(QStringLiteral(QML_DIR "/TrustCard.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(!content.contains(QStringLiteral("qr_code_scanner")));

        QRegularExpression qsTrCall(QStringLiteral("qsTr\\(\"([^\"]*)\""));
        auto it = qsTrCall.globalMatch(content);
        while (it.hasNext()) {
            const QString text = it.next().captured(1);
            QVERIFY2(!text.toLower().contains(QStringLiteral("qr"))
                     && !text.toLower().contains(QStringLiteral("scan")),
                     qPrintable(text));
        }
    }

    void neverTouchesAppOrInventsTrust()
    {
        QFile file(QStringLiteral(QML_DIR "/TrustCard.qml"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QString content = QString::fromUtf8(file.readAll());
        QVERIFY(!content.contains(QStringLiteral("app.")));
        // Brand tokens only — no stray hex outside AppTheme.trust*.
        QRegularExpression hexColor(QStringLiteral("color\\s*:\\s*\"#[0-9A-Fa-f]{3,8}\""));
        QVERIFY(!content.contains(hexColor));
        QVERIFY(!content.contains(QStringLiteral("Qt.rgba(")));
    }

    void onlyVerifyActionExistsNoMessageButton()
    {
        auto *card = find(QStringLiteral("card"));
        QVERIFY(card);
        auto *verifyButton = find(QStringLiteral("trustCardVerifyButton"));
        QVERIFY(verifyButton);
        QVERIFY(verifyButton->property("visible").toBool());

        bool foundMessageButton = false;
        for (QObject *candidate : card->findChildren<QObject *>()) {
            if (candidate->property("text").toString() == QStringLiteral("Message")) {
                foundMessageButton = true;
                break;
            }
        }
        QVERIFY(!foundMessageButton);

        card->setProperty("showVerify", false);
        QTRY_VERIFY(!verifyButton->property("visible").toBool());
        card->setProperty("showVerify", true);
        QTRY_VERIFY(verifyButton->property("visible").toBool());
    }

    void verifyClickEmitsVerifyRequested()
    {
        auto *root = m_root;
        QSignalSpy spy(root, SIGNAL(verifySignal()));
        auto *verifyButton = find(QStringLiteral("trustCardVerifyButton"));
        QVERIFY(verifyButton);
        QMetaObject::invokeMethod(verifyButton, "clicked");
        QCOMPARE(spy.count(), 1);
    }

    void completeAndPendingNodesRenderDistinctly()
    {
        auto *repeater = find(QStringLiteral("trustChainStepRepeater"));
        QVERIFY(repeater);
        QCOMPARE(repeater->property("count").toInt(), 3);

        QQuickItem *step0 = nullptr;
        QMetaObject::invokeMethod(repeater, "itemAt",
                                  Q_RETURN_ARG(QQuickItem *, step0), Q_ARG(int, 0));
        QVERIFY(step0);
        auto *fill0 = step0->findChild<QQuickItem *>(QStringLiteral("trustNodeFill"));
        QVERIFY(fill0);
        QVERIFY(fill0->property("visible").toBool());
        QCOMPARE(fill0->property("color").value<QColor>(), token("tokTrustYellow"));
        auto *dash0 = step0->findChild<QQuickItem *>(QStringLiteral("trustNodeDashRing"));
        QVERIFY(dash0);
        QVERIFY(!dash0->property("visible").toBool());

        QQuickItem *step2 = nullptr;
        QMetaObject::invokeMethod(repeater, "itemAt",
                                  Q_RETURN_ARG(QQuickItem *, step2), Q_ARG(int, 2));
        QVERIFY(step2);
        auto *fill2 = step2->findChild<QQuickItem *>(QStringLiteral("trustNodeFill"));
        QVERIFY(fill2);
        QVERIFY(!fill2->property("visible").toBool());
        auto *dash2 = step2->findChild<QQuickItem *>(QStringLiteral("trustNodeDashRing"));
        QVERIFY(dash2);
        QVERIFY(dash2->property("visible").toBool());
    }

    void connectorIsCompleteOnlyWhenBothEndsAreComplete()
    {
        auto *repeater = find(QStringLiteral("trustChainStepRepeater"));
        QVERIFY(repeater);

        // steps[0]/[1] both complete -> connector 0 is yellow.
        QQuickItem *step0 = nullptr;
        QMetaObject::invokeMethod(repeater, "itemAt",
                                  Q_RETURN_ARG(QQuickItem *, step0), Q_ARG(int, 0));
        QVERIFY(step0);
        auto *connector0 = step0->findChild<QQuickItem *>(QStringLiteral("trustChainConnector"));
        QVERIFY(connector0);
        QCOMPARE(connector0->property("color").value<QColor>(), token("tokTrustYellow"));

        // steps[1] complete, steps[2] pending -> connector 1 is pending.
        QQuickItem *step1 = nullptr;
        QMetaObject::invokeMethod(repeater, "itemAt",
                                  Q_RETURN_ARG(QQuickItem *, step1), Q_ARG(int, 1));
        QVERIFY(step1);
        auto *connector1 = step1->findChild<QQuickItem *>(QStringLiteral("trustChainConnector"));
        QVERIFY(connector1);
        QCOMPARE(connector1->property("color").value<QColor>(), token("tokTrustPending"));
    }

    void brandFaceIsUsedForTheDisplayName()
    {
        auto *card = find(QStringLiteral("card"));
        QVERIFY(card);
        QQuickItem *nameLabel = nullptr;
        for (QObject *candidate : card->findChildren<QObject *>()) {
            if (candidate->property("text").toString() == QStringLiteral("Alice Example")) {
                nameLabel = qobject_cast<QQuickItem *>(candidate);
                break;
            }
        }
        QVERIFY(nameLabel);
        const QFont font = nameLabel->property("font").value<QFont>();
        QCOMPARE(font.family(), QStringLiteral("Space Grotesk"));
        QCOMPARE(font.pixelSize(), 17);
    }

    void pendingRingActuallyPaintsPixels()
    {
        // The dashed pending ring must RENDER, not merely exist: the
        // cooperative Canvas strategy once deferred the first paint past
        // offscreen grabs, shipping ringless captures while the item's
        // `visible` property (all the old assertion checked) stayed true.
        const QImage shot = m_window->grabWindow();
        QVERIFY(!shot.isNull());
        // Repeater delegates are not QObject-parented into the window tree,
        // so resolve through the repeater and search WITHIN each delegate
        // subtree (the canvas is a static child there).
        auto *repeater = find(QStringLiteral("trustChainStepRepeater"));
        QVERIFY(repeater);
        QQuickItem *ring = nullptr;
        const int steps = repeater->property("count").toInt();
        for (int i = 0; i < steps && !ring; ++i) {
            QQuickItem *step = nullptr;
            QMetaObject::invokeMethod(repeater, "itemAt",
                                      Q_RETURN_ARG(QQuickItem *, step),
                                      Q_ARG(int, i));
            if (!step)
                continue;
            auto *candidate = step->findChild<QQuickItem *>(
                QStringLiteral("trustNodeDashRing"));
            if (candidate && candidate->isVisible())
                ring = candidate;
        }
        QVERIFY2(ring, "no visible pending-node ring canvas");
        const QPointF tl = ring->mapToScene(QPointF(0, 0));
        const int w = int(ring->width());
        const int h = int(ring->height());
        QVERIFY(w >= 20 && h >= 20);
        // Canvas corners sit outside the circle — module background.
        const QColor bg = shot.pixelColor(int(tl.x()) + 1, int(tl.y()) + 1);
        const qreal cx = w / 2.0, cy = h / 2.0;
        int inkedBandPixels = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const qreal dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                const qreal r = std::sqrt(dx * dx + dy * dy);
                if (r < 8.0 || r > 12.5)
                    continue; // only the ring annulus, not the icon
                const QColor px = shot.pixelColor(int(tl.x()) + x,
                                                  int(tl.y()) + y);
                const int delta = qMax(qMax(qAbs(px.red() - bg.red()),
                                            qAbs(px.green() - bg.green())),
                                       qAbs(px.blue() - bg.blue()));
                if (delta > 20)
                    ++inkedBandPixels;
            }
        }
        QVERIFY2(inkedBandPixels >= 20,
                 qPrintable(QStringLiteral("ring band has only %1 inked px")
                                .arg(inkedBandPixels)));
    }

    void brandColoursStayFixedAcrossThemeChanges()
    {
        const QColor navyBefore = token(QStringLiteral("tokTrustNavy"));
        m_root->setProperty("themeMode", 8); // Moss Light
        QCoreApplication::processEvents();
        QCOMPARE(token(QStringLiteral("tokTrustNavy")), navyBefore);
        m_root->setProperty("themeMode", 10); // Deep Teal
        QCoreApplication::processEvents();
        QCOMPARE(token(QStringLiteral("tokTrustNavy")), navyBefore);
        m_root->setProperty("themeMode", 9);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    TrustCardTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "TrustCardTest.moc"
