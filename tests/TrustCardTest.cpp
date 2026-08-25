// v0.6.5 (SPEC 1r): offscreen proof for the standalone TrustCard component.
// TrustCard never touches `app.*` (the embedding surface owns all real
// bindings), so this test drives it purely through property injection:
// complete vs pending node rendering, the both-ends-complete connector rule,
// the Verify-only action (never Message, never QR), and — since 2026-08-26 —
// that the card FOLLOWS the selected theme.
//
// That last group replaces brandColoursStayFixedAcrossThemeChanges(), which
// asserted the opposite (tokTrustNavy identical across theme modes 8, 9 and
// 10) for as long as the card was pinned to the raw Storm literals. The
// maintainer reported the consequence: "the blue lightning session status
// should match the rest of the theme". The three replacements below each
// FAIL on the unfixed tree, and it is worth saying how, because a test that
// only passes after the fix is not the same as a test that fails before it:
//   cardRetintsWhenTheThemeChanges          — every sample was one constant,
//                                             so each QVERIFY(a != b) fails.
//   cardPaintsTheSettingsCardPairOnEveryTheme — the card was _stoPanel /
//                                             _stoBorder while stormCanvas /
//                                             stormBorder route per theme, so
//                                             it fails on all ten legacy
//                                             themes AND on Storm (_stoPanel
//                                             is not _stoCanvas).
//   stormKeepsTheBrandLiterals              — under Storm the old card fill
//                                             was #202473 and the complete
//                                             node ink was #202473 too; the
//                                             expected values are #121655 and
//                                             #0A0F24.

#include <QtTest/QtTest>

#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlProperty>
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

    // The card owns no colour tokens of its own any more; these probes name
    // the routed storm* roles it actually reads, so a mis-routed token shows
    // up as a colour mismatch rather than as an invalid QColor.
    Rectangle { objectName: "tokBolt"; visible: false; color: AppTheme.bolt }
    Rectangle { objectName: "tokBoltInk"; visible: false; color: AppTheme.boltInk }
    Rectangle { objectName: "tokPending"; visible: false; color: AppTheme.stormBorderStrong }
    Rectangle { objectName: "tokCanvas"; visible: false; color: AppTheme.stormCanvas }
    Rectangle { objectName: "tokBorder"; visible: false; color: AppTheme.stormBorder }
    Rectangle { objectName: "tokInset"; visible: false; color: AppTheme.stormInset }
    Rectangle { objectName: "tokText"; visible: false; color: AppTheme.stormText }
    Rectangle { objectName: "tokTextSecondary"; visible: false; color: AppTheme.stormTextSecondary }
    Rectangle { objectName: "tokTextMuted"; visible: false; color: AppTheme.stormTextMuted }

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
    // Rectangle::border is a QQuickPen, so border.color has to be read
    // through QQmlProperty rather than off the QObject directly.
    static QColor borderColor(QObject *item)
    {
        return item ? QQmlProperty::read(item, QStringLiteral("border.color"))
                          .value<QColor>()
                    : QColor();
    }
    void setTheme(int mode)
    {
        m_root->setProperty("themeMode", mode);
        QCoreApplication::processEvents();
    }
    // The complete node's icon, resolved through the repeater (delegates are
    // not QObject-parented into the window tree).
    QQuickItem *stepChild(int step, const QString &name) const
    {
        auto *repeater = find(QStringLiteral("trustChainStepRepeater"));
        if (!repeater)
            return nullptr;
        QQuickItem *item = nullptr;
        QMetaObject::invokeMethod(repeater, "itemAt",
                                  Q_RETURN_ARG(QQuickItem *, item),
                                  Q_ARG(int, step));
        return item ? item->findChild<QQuickItem *>(name) : nullptr;
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
        QCOMPARE(fill0->property("color").value<QColor>(), token("tokBolt"));
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
        QCOMPARE(connector0->property("color").value<QColor>(), token("tokBolt"));

        // steps[1] complete, steps[2] pending -> connector 1 is pending.
        QQuickItem *step1 = nullptr;
        QMetaObject::invokeMethod(repeater, "itemAt",
                                  Q_RETURN_ARG(QQuickItem *, step1), Q_ARG(int, 1));
        QVERIFY(step1);
        auto *connector1 = step1->findChild<QQuickItem *>(QStringLiteral("trustChainConnector"));
        QVERIFY(connector1);
        QCOMPARE(connector1->property("color").value<QColor>(), token("tokPending"));
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

    void nodeIconInkIsTheInkForItsOwnFill()
    {
        // The one site where a mechanical token swap would have been wrong.
        // The complete node's glyph sits ON the bolt disc, so it must be
        // boltInk — CLAUDE.md §7, "ink on a bolt/accent fill uses boltInk,
        // never stormPanel". It read correctly for years only because the
        // pinned card fill happened to be navy; routed unchanged it would
        // have painted the PAGE GROUND onto a yellow disc.
        //
        // The pending glyph was borderStrong, which measures 1.76-3.50:1 on
        // inputBackground across the eleven themes — an illegible 12px icon.
        // stormTextMuted is AA-covered on that fill on every theme.
        //
        // On the unfixed tree both QCOMPAREs fail: the complete ink was
        // trustNavy (_stoPanel) and the pending ink was trustPending
        // (_stoBorderStrong).
        setTheme(9);
        auto *complete = stepChild(0, QStringLiteral("trustNodeIcon"));
        QVERIFY(complete);
        QCOMPARE(complete->property("color").value<QColor>(),
                 token(QStringLiteral("tokBoltInk")));
        auto *pending = stepChild(2, QStringLiteral("trustNodeIcon"));
        QVERIFY(pending);
        QCOMPARE(pending->property("color").value<QColor>(),
                 token(QStringLiteral("tokTextMuted")));
    }

    void cardRetintsWhenTheThemeChanges()
    {
        // The property the maintainer reported, stated directly: switching
        // theme must MOVE the card's colours. Sampled through the real
        // AppTheme.mode binding and off the card's own items — not by
        // calling a routing helper, which would prove only that the helper
        // works and nothing about whether the card reaches it.
        //
        // Unfixed tree: every one of these samples was a constant, so the
        // first QVERIFY fires.
        auto *surface = find(QStringLiteral("trustCardSurface"));
        auto *chain = find(QStringLiteral("trustChainPanel"));
        QVERIFY(surface);
        QVERIFY(chain);

        setTheme(8); // Moss Light
        const QColor fillLight = surface->property("color").value<QColor>();
        const QColor edgeLight = borderColor(surface);
        const QColor chainLight = chain->property("color").value<QColor>();
        const QColor inkLight = token(QStringLiteral("tokText"));
        const QColor boltLight = token(QStringLiteral("tokBolt"));

        setTheme(10); // Deep Teal
        QVERIFY2(surface->property("color").value<QColor>() != fillLight,
                 "the card fill must retint with the theme");
        QVERIFY2(borderColor(surface) != edgeLight,
                 "the card border must retint with the theme");
        QVERIFY2(chain->property("color").value<QColor>() != chainLight,
                 "the trust-chain panel must retint with the theme");
        QVERIFY2(token(QStringLiteral("tokText")) != inkLight,
                 "the display-name ink must retint with the theme");
        QVERIFY2(token(QStringLiteral("tokBolt")) != boltLight,
                 "the complete-state accent must retint with the theme");
        setTheme(9);
    }

    void cardPaintsTheSettingsCardPairOnEveryTheme()
    {
        // "Doesn't match the theme" is really "doesn't match its siblings":
        // SettingsScreen.qml's SettingsCard paints stormCanvas with a
        // stormBorder edge, and the trust card sits in the same column. So
        // assert the same pair on all eleven modes rather than one.
        //
        // Unfixed tree: the card was _stoPanel/_stoBorder, so this fails on
        // the ten legacy themes AND on Storm, where stormCanvas is _stoCanvas
        // (#121655) and the pinned fill was _stoPanel (#202473).
        auto *surface = find(QStringLiteral("trustCardSurface"));
        auto *chain = find(QStringLiteral("trustChainPanel"));
        QVERIFY(surface);
        QVERIFY(chain);
        for (int mode = 1; mode <= 11; ++mode) {
            setTheme(mode);
            QCOMPARE(surface->property("color").value<QColor>(),
                     token(QStringLiteral("tokCanvas")));
            QCOMPARE(borderColor(surface), token(QStringLiteral("tokBorder")));
            // The inner module rides the input-fill rung on every theme, so
            // it stays a distinct surface from the card ground. The
            // inequality guards the routing, not the SIZE of the step:
            // Graphite's two rungs are one unit apart by design.
            QCOMPARE(chain->property("color").value<QColor>(),
                     token(QStringLiteral("tokInset")));
            QCOMPARE(borderColor(chain), token(QStringLiteral("tokBorder")));
            QVERIFY2(chain->property("color").value<QColor>()
                         != surface->property("color").value<QColor>(),
                     qPrintable(QStringLiteral("the chain panel resolved to "
                                               "the card ground on theme %1")
                                    .arg(mode)));
        }
        setTheme(9);
    }

    void stormKeepsTheBrandLiterals()
    {
        // Under Storm (theme 11) every routed role the card reads must land
        // on its SPEC §1 literal — the routing must not quietly re-colour the
        // brand theme on its way to fixing the other ten.
        //
        // Two values under Storm DID move, deliberately, and are asserted at
        // their new values rather than hidden:
        //   * the card fill, _stoPanel #202473 -> _stoCanvas #121655, so the
        //     card matches the SettingsCards beside it on Storm too;
        //   * the watermark, a 10%-opacity bolt -> AppTheme.stormWatermark
        //     (12% alpha), the token IdentityCard and MemberProfilePopover
        //     already use for the same hero-card glyph. Not asserted here —
        //     it is an alpha on a decorative glyph, and there is no probe
        //     that could distinguish it from the fill behind it.
        // Everything else below is byte-identical to the deleted pin.
        setTheme(11);
        auto *surface = find(QStringLiteral("trustCardSurface"));
        auto *chain = find(QStringLiteral("trustChainPanel"));
        QVERIFY(surface);
        QVERIFY(chain);
        QCOMPARE(surface->property("color").value<QColor>(), QColor("#121655"));
        QCOMPARE(borderColor(surface), QColor("#303C80"));
        QCOMPARE(chain->property("color").value<QColor>(), QColor("#0A112E"));
        QCOMPARE(token(QStringLiteral("tokBolt")), QColor("#FFD447"));
        QCOMPARE(token(QStringLiteral("tokBoltInk")), QColor("#0A0F24"));
        QCOMPARE(token(QStringLiteral("tokPending")), QColor("#434F9D"));
        QCOMPARE(token(QStringLiteral("tokText")), QColor("#F2F4FF"));
        QCOMPARE(token(QStringLiteral("tokTextSecondary")), QColor("#C9D2F2"));
        QCOMPARE(token(QStringLiteral("tokTextMuted")), QColor("#9CA3D2"));
        setTheme(9);
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    TrustCardTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "TrustCardTest.moc"
