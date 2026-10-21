// Theme editor, swarm rounds 1-2 (C85). Two kinds of case. The source scans
// name a defect the engine cannot reach without a live window and assert the
// shape of its fix. The ENGINE cases load the real components against the
// mock backend and drive the logic that the scans only grep for: Fix on a
// gradient backdrop, Fix on the accent label, and removing a gradient stop.
#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QSignalSpy>
#include <memory>
#include <QRegularExpression>
#include <QString>
#include <QtTest/QtTest>

#include "app/AppController.h"
#include "auth/AuthManager.h"

namespace {

// The theme editor, plus the three Bindings Main.qml keeps between the store
// and AppTheme (the dialog grades AppTheme's resolved palette).
const char *kHost = R"QML(
import QtQuick
import MatrixClient
Item {
    id: host
    property alias dlg: ed
    Binding { target: AppTheme; property: "customOverrides"; value: app.customTheme.colors }
    Binding { target: AppTheme; property: "customBase"; value: app.customTheme.baseTheme }
    Binding { target: AppTheme; property: "customGradients"; value: app.customTheme.gradients }
    ThemeEditorDialog { id: ed }
    // "fg/bg" of every audit row that fails right now.
    function failingRows() {
        var rows = app.customTheme.auditWithGradients(ed.auditPalette,
                       app.customTheme.gradients)
        var out = []
        for (var i = 0; i < rows.length; ++i)
            if (rows[i].passes === false)
                out.push(rows[i].fg + "/" + rows[i].bg)
        return out
    }
}
)QML";

QString read(const QString &name)
{
    QFile file(QStringLiteral(QML_DIR "/") + name);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}

QString body(const QString &source, const QString &marker, int length = 1500)
{
    const int at = source.indexOf(marker);
    return at < 0 ? QString() : source.mid(at, length);
}

} // namespace

class ThemeEditorRound1ContractTest : public QObject
{
    Q_OBJECT

    // Everything an engine case needs, owned for the case's length.
    struct Rig {
        AppController controller{AppController::MockBackend};
        QQmlApplicationEngine engine;
        QStringList warnings;
        QObject *host = nullptr;
        std::unique_ptr<QQmlComponent> component;
    };
    bool boot(Rig &r, const QByteArray &qml)
    {
        QSignalSpy login(r.controller.auth(), &AuthManager::loginSucceeded);
        r.controller.auth()->login(QStringLiteral("https://mock.local"),
                                   QStringLiteral("alice"), QStringLiteral("unused"));
        if (!login.wait(3000))
            return false;
        r.engine.rootContext()->setContextProperty("app", &r.controller);
        connect(&r.engine, &QQmlEngine::warnings, this,
                [&r](const QList<QQmlError> &errors) {
                    for (const QQmlError &e : errors)
                        r.warnings << e.toString();
                });
        r.component = std::make_unique<QQmlComponent>(&r.engine);
        r.component->setData(qml, QUrl(QStringLiteral("qrc:/host.qml")));
        r.host = r.component->create();
        if (!r.host)
            qWarning().noquote() << r.component->errorString();
        return r.host != nullptr;
    }
    static bool setColor(QObject *store, const char *role, const char *hex)
    {
        bool ok = false;
        QMetaObject::invokeMethod(store, "setColor", Q_RETURN_ARG(bool, ok),
                                  Q_ARG(QString, QString::fromLatin1(role)),
                                  Q_ARG(QString, QString::fromLatin1(hex)));
        return ok;
    }
    static bool setGradient(QObject *store, const char *role, const QStringList &stops)
    {
        const QVariantMap spec{ { QStringLiteral("type"), QStringLiteral("linear") },
                                { QStringLiteral("angle"), 180 },
                                { QStringLiteral("stops"), stops } };
        bool ok = false;
        QMetaObject::invokeMethod(store, "setGradient", Q_RETURN_ARG(bool, ok),
                                  Q_ARG(QString, QString::fromLatin1(role)),
                                  Q_ARG(QVariantMap, spec));
        return ok;
    }
    static QQuickItem *itemNamed(QQuickItem *at, const QString &name)
    {
        if (at->objectName() == name)
            return at;
        const auto kids = at->childItems();
        for (QQuickItem *k : kids)
            if (QQuickItem *hit = itemNamed(k, name))
                return hit;
        return nullptr;
    }
    static QStringList failing(QObject *host)
    {
        QVariant v;
        QMetaObject::invokeMethod(host, "failingRows", Q_RETURN_ARG(QVariant, v));
        return v.toStringList();
    }

private slots:
    // ENGINE. C85-F04: Fix graded a role against the FLAT colour a gradient
    // paints over, so a label that fails on the gradient was told "nothing
    // to fix". The room list is flat light with a dark gradient on it and the
    // room name is dark: flat grading passes, drawn grading fails. Fix must
    // move the name to something every stop clears.
    void fixGradesAgainstTheGradientThatIsDrawn()
    {
        Rig r;
        QVERIFY(boot(r, kHost));
        QObject *store = r.controller.customTheme();
        QMetaObject::invokeMethod(store, "createTheme",
                                  Q_ARG(QString, QStringLiteral("round2")));
        QVERIFY(setColor(store, "sidebar", "#F0F0F0"));
        QVERIFY(setGradient(store, "sidebar",
                            { QStringLiteral("#10101A"), QStringLiteral("#2A2A44") }));
        QVERIFY(setColor(store, "roomName", "#202020"));

        QObject *dlg = r.host->property("dlg").value<QObject *>();
        QVERIFY(dlg);
        const QStringList before = failing(r.host);
        QVERIFY2(before.contains(QStringLiteral("roomName/roomList")),
                 qPrintable(before.join(u',')));

        QMetaObject::invokeMethod(dlg, "makeReadable",
                                  Q_ARG(QVariant, QStringLiteral("roomName")));
        const QString now = store->property("colors").toMap()
                                .value(QStringLiteral("roomName")).toString();
        QVERIFY2(now.toUpper() != QStringLiteral("#202020"),
                 "Fix did nothing for a label that fails on the gradient");
        for (const QString &stop : { QStringLiteral("#10101A"), QStringLiteral("#2A2A44") }) {
            double ratio = 0;
            QMetaObject::invokeMethod(store, "contrastRatio", Q_RETURN_ARG(double, ratio),
                                      Q_ARG(QString, now), Q_ARG(QString, stop));
            QVERIFY2(ratio >= 4.5, qPrintable(QStringLiteral("%1 on %2 is %3")
                                                  .arg(now, stop).arg(ratio)));
        }
        QVERIFY2(!failing(r.host).contains(QStringLiteral("roomName/roomList")),
                 "the row Fix was pressed on still fails");
    }

    // ENGINE. C85-F05: Fix on the accent label graded only that one pair, and
    // the hover and pressed labels that follow it were pushed under their bar.
    // After a Fix no row may fail that did not fail before, whether the fix
    // can satisfy every bar (green accent) or none can (a dark accent under
    // light hover fills: Fix must then decline rather than trade one failure
    // for two).
    void fixOnTheAccentLabelAddsNoFailingRow_data()
    {
        QTest::addColumn<QString>("accent");
        QTest::addColumn<QString>("ink");
        QTest::addColumn<bool>("mustClearAccentRow");
        QTest::newRow("a fix exists") << "#208060" << "#808080" << true;
        QTest::newRow("no fix exists") << "#5030A0" << "#303030" << false;
    }
    void fixOnTheAccentLabelAddsNoFailingRow()
    {
        QFETCH(QString, accent);
        QFETCH(QString, ink);
        QFETCH(bool, mustClearAccentRow);
        Rig r;
        QVERIFY(boot(r, kHost));
        QObject *store = r.controller.customTheme();
        QMetaObject::invokeMethod(store, "createTheme",
                                  Q_ARG(QString, QStringLiteral("round2b")));
        QVERIFY(setColor(store, "accent", accent.toLatin1().constData()));
        QVERIFY(setColor(store, "accentText", ink.toLatin1().constData()));
        QObject *dlg = r.host->property("dlg").value<QObject *>();
        const QStringList before = failing(r.host);
        QVERIFY2(before.contains(QStringLiteral("accentText/accent")),
                 qPrintable(before.join(u',')));

        QMetaObject::invokeMethod(dlg, "makeReadable",
                                  Q_ARG(QVariant, QStringLiteral("accentText")));
        const QStringList after = failing(r.host);
        QStringList added;
        for (const QString &row : after)
            if (!before.contains(row))
                added << row;
        QVERIFY2(added.isEmpty(),
                 qPrintable(QStringLiteral("Fix introduced failing rows: ")
                            + added.join(u',')));
        if (mustClearAccentRow)
            QVERIFY2(!after.contains(QStringLiteral("accentText/accent")),
                     "the row Fix was pressed on still fails");
    }

    // ENGINE. C85-F03: Remove on a gradient stop looked `editor` up AFTER the
    // write destroyed its delegate, which logged a ReferenceError and never
    // re-pointed the picker.
    void removingAStopReselectsWithoutAReferenceError()
    {
        Rig r;
        static const char *kEditor = R"QML(
import QtQuick
import MatrixClient
Item { property alias ge: g; GradientEditor { id: g; role: "background" } }
)QML";
        QVERIFY(boot(r, kEditor));
        QObject *store = r.controller.customTheme();
        QMetaObject::invokeMethod(store, "createTheme",
                                  Q_ARG(QString, QStringLiteral("round2c")));
        QVERIFY(setGradient(store, "background", { QStringLiteral("#102030"), QStringLiteral("#203040"),
                                     QStringLiteral("#304050") }));
        QObject *ge = r.host->property("ge").value<QObject *>();
        QVERIFY(ge);
        QTRY_COMPARE(ge->property("stopCount").toInt(), 3);
        QSignalSpy selected(ge, SIGNAL(stopSelected(int)));
        QObject *remove = nullptr;
        QTRY_VERIFY((remove = itemNamed(qobject_cast<QQuickItem *>(ge),
                         QStringLiteral("gradientStopRemove_1"))) != nullptr);
        r.warnings.clear();
        QMetaObject::invokeMethod(remove, "clicked");
        QCOMPARE(ge->property("stopCount").toInt(), 2);
        for (const QString &w : std::as_const(r.warnings))
            QVERIFY2(!w.contains(QStringLiteral("not defined")), qPrintable(w));
        QVERIFY2(!selected.isEmpty(), "the picker was never re-pointed at a surviving stop");
        QCOMPARE(selected.last().at(0).toInt(), 1);
    }

    // C85-F03: the delegate (and with it `editor`) is destroyed by the write.
    void removingAStopDoesNotLookTheEditorUpAfterTheWrite()
    {
        const QString src = read(QStringLiteral("GradientEditor.qml"));
        const QString at = body(src, QStringLiteral("var removed = stopRow.index"), 500);
        QVERIFY2(!at.isEmpty(), "the Remove handler moved");
        const int write = at.indexOf(QStringLiteral("removeStopAt"));
        QVERIFY(write > 0);
        QVERIFY2(!at.mid(write).contains(QRegularExpression(QStringLiteral("\\beditor\\."))),
                 "`editor` is looked up after removeStopAt destroyed the delegate");
    }

    // C85-F01: a flat role must not inherit the backdrop's stop.
    void aFlatRoleNeverGetsAStopFromTheHeldGradientEditor()
    {
        const QString src = read(QStringLiteral("ThemeEditorDialog.qml"));
        const QString at = body(src, QStringLiteral("id: gradientEditor"), 1800);
        QVERIFY(at.contains(QStringLiteral("autoSelect: root.editingTakesGradient")));
        const QString handler = body(at, QStringLiteral("onStopSelected"), 300);
        QVERIFY2(handler.contains(QStringLiteral("!root.editingTakesGradient")),
                 "onStopSelected accepts a stop for a role with no gradient");
    }

    // C85-F07 / F09
    void redoAndFindWorkWhereTheyWereSilent()
    {
        const QString src = read(QStringLiteral("ThemeEditorDialog.qml"));
        QVERIFY2(src.contains(QStringLiteral("[StandardKey.Redo, \"Ctrl+Y\"]")),
                 "Ctrl+Y does not redo");
        const QString find = body(src, QStringLiteral("sequences: [StandardKey.Find]"), 500);
        QVERIFY(!find.contains(QStringLiteral("!root.pickerPanelCovers")));
        QVERIFY(find.contains(QStringLiteral("root.editingRole = \"\"")));
    }

    // C85-F04 / F05: Fix grades what is drawn, and never trades one failure
    // for another.
    void makeReadableCountsGradientStopsAndSiblingStates()
    {
        const QString src = read(QStringLiteral("ThemeEditorDialog.qml"));
        const QString fn = body(src, QStringLiteral("function makeReadable"), 3000);
        QVERIFY(fn.contains(QStringLiteral("readableConstraints")));
        QVERIFY(fn.contains(QStringLiteral("betterWithoutNewFailures(current, next")));
        const QString scope = body(src, QStringLiteral("function readableConstraints"), 1200);
        QVERIFY(scope.contains(QStringLiteral("dependentsOf(key)")));
        QVERIFY(scope.contains(QStringLiteral("drawnColoursOf")));
        // never silent on a Fix that does nothing
        QVERIFY(fn.contains(QStringLiteral("already clears")));
    }

    // C85-F06 / D01 / F08 / D02 / D04 / D05
    void headerWrapsAndEveryFocusAndGroupsGapIsClosed()
    {
        const QString src = read(QStringLiteral("ThemeEditorDialog.qml"));
        QVERIFY(src.contains(QStringLiteral("readonly property bool narrowHeader")));
        QVERIFY(src.contains(QStringLiteral("Layout.columnSpan: root.narrowHeader")));
        const QString btn = body(src, QStringLiteral("component EditorButton"), 2200);
        QVERIFY(btn.contains(QStringLiteral("btn.primary ? AppTheme.editorAccentInk")));
        QVERIFY(btn.contains(QStringLiteral("Keys.onReturnPressed")));
        const QString panel = read(QStringLiteral("ThemeReadabilityPanel.qml"));
        QVERIFY2(panel.contains(QStringLiteral("Keys.onTabPressed"))
                     && panel.contains(QStringLiteral("Keys.onBacktabPressed"))
                     && panel.contains(QStringLiteral("refocusAfterRebuild")),
                 "the readability panel no longer keeps Tab inside itself");
        QVERIFY2(body(src, QStringLiteral("function dismissTransient"), 400)
                     .contains(QStringLiteral("readabilityBadge.forceActiveFocus()")),
                 "Escape no longer returns focus to the verdict chip");
        QVERIFY(src.contains(QStringLiteral("onActivatedAmbiguously: root.redoEdit()")));
        const QString hex = body(src, QStringLiteral("objectName: \"themeRoleHex_\""), 3500);
        QVERIFY(hex.contains(QStringLiteral("elide: Label.ElideRight")));
        QVERIFY2(src.contains(QStringLiteral("was reset.")),
                 "the picker reset no longer announces an undo");
    }
};

QTEST_MAIN(ThemeEditorRound1ContractTest)
#include "ThemeEditorRound1ContractTest.moc"
