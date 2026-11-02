
#include <QtTest/QtTest>

#include <QColor>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QVariantMap>

#include "app/AppController.h"

namespace {

struct ThemeCase {
    int id;
    const char *name;
    bool light;
};
const ThemeCase kThemes[] = {
    { 11, "storm", false },
    { 8, "moss-light", true },
    { 9, "indigo-night", false },
    { 10, "deep-teal", false },
    { 1, "lightning-light", true },
    { 4, "midnight", false },
};

QString snapshotDir()
{
    QString dir = qEnvironmentVariable("LIGHTNING_SNAPSHOT_DIR");
    if (dir.isEmpty())
        dir = QDir::current().filePath(QStringLiteral("snapshots"));
    QDir().mkpath(dir);
    return dir;
}

QColor sampleAvg(const QImage &img, const QRect &r)
{
    long long rs = 0, gs = 0, bs = 0, n = 0;
    const QRect clip = r.intersected(img.rect());
    for (int y = clip.top(); y <= clip.bottom(); ++y) {
        for (int x = clip.left(); x <= clip.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            rs += c.red(); gs += c.green(); bs += c.blue(); ++n;
        }
    }
    if (n == 0)
        return {};
    return QColor(int(rs / n), int(gs / n), int(bs / n));
}

int channelDelta(const QColor &a, const QColor &b)
{
    return qMax(qMax(qAbs(a.red() - b.red()), qAbs(a.green() - b.green())),
                qAbs(a.blue() - b.blue()));
}

}

class ThemeSnapshotTest : public QObject
{
    Q_OBJECT

private:
    QQmlEngine m_engine;

    QObject *buildWindow(int themeId)
    {
        const char *qml = R"QML(
import QtQuick
import QtQuick.Controls
import MatrixClient

ApplicationWindow {
    id: win
    width: 300; height: 360
    property int themeId: 0
    Component.onCompleted: AppTheme.mode = themeId
    color: AppTheme.background

    // Resolved tokens exposed for the C++ side to build the app palette
    // (mirrors Main.qml's syncControlPalette map).
    property color tokWindow: AppTheme.background
    property color tokWindowText: AppTheme.textPrimary
    property color tokBase: AppTheme.inputBackground
    property color tokButton: AppTheme.cardElevated
    property color tokButtonText: AppTheme.textPrimary
    property color tokHighlight: AppTheme.selected
    property color tokHighlightedText: AppTheme.selectedText

    // Mirror Main.qml so Fusion popups inherit the themed palette.
    palette {
        window: AppTheme.background
        windowText: AppTheme.textPrimary
        base: AppTheme.inputBackground
        alternateBase: AppTheme.cardElevated
        text: AppTheme.textPrimary
        button: AppTheme.cardElevated
        buttonText: AppTheme.textPrimary
        highlight: AppTheme.selected
        highlightedText: AppTheme.selectedText
        placeholderText: AppTheme.textMuted
        toolTipBase: AppTheme.cardElevated
        toolTipText: AppTheme.textPrimary
        light: AppTheme.hover
        midlight: AppTheme.border
        mid: AppTheme.borderStrong
        dark: AppTheme.textSecondary
        brightText: AppTheme.accentText
        link: AppTheme.link
    }

    Rectangle { objectName: "swatchSidebar"; x: 0;  y: 0;  width: 60; height: 60; color: AppTheme.sidebar }
    Rectangle { objectName: "swatchSurface"; x: 60; y: 0;  width: 60; height: 60; color: AppTheme.surface }
    Rectangle { objectName: "swatchAccent";  x: 120;y: 0;  width: 60; height: 60; color: AppTheme.accent }
    Rectangle { objectName: "swatchHover";   x: 180;y: 0;  width: 60; height: 60; color: AppTheme.hover }

    // Initials avatar on its deterministic palette colour.
    Rectangle {
        objectName: "avatarSwatch"
        x: 240; y: 0; width: 60; height: 60
        radius: width / 2
        // The discs are derived from the active theme's accent now, so the
        // probe asks for one the way the application does — by identity key,
        // with the ink the disc can carry rather than a fixed white.
        color: AppTheme.avatarColor("!probe:example.org")
        Text {
            anchors.centerIn: parent; text: "AB"; font.bold: true
            color: AppTheme.avatarInk("!probe:example.org")
        }
    }

    Button {
        objectName: "probeButton"
        x: 10; y: 80; width: 120; height: 34
        text: "Button"
    }
    TextField {
        objectName: "probeField"
        x: 10; y: 124; width: 200; height: 32
        text: "field"
    }
    ComboBox {
        id: combo
        objectName: "probeCombo"
        x: 10; y: 166; width: 200; height: 32
        model: [ "One", "Two", "Three" ]
    }
}
)QML";
        auto *component = new QQmlComponent(&m_engine);
        component->setData(QByteArray(qml), QUrl(QStringLiteral("qrc:/probe.qml")));
        if (component->isError()) {
            qWarning() << component->errorString();
            delete component;
            return nullptr;
        }
        QObject *obj = component->createWithInitialProperties(
            { { QStringLiteral("themeId"), themeId } });
        if (!obj) {
            delete component;
            return nullptr;
        }
        component->setParent(obj);
        return obj;
    }

    QQuickItem *childByName(QObject *root, const QString &name)
    {
        return root->findChild<QQuickItem *>(name);
    }

private Q_SLOTS:
    void eachThemePaintsItsOwnTokens_data()
    {
        QTest::addColumn<int>("themeId");
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("light");
        for (const ThemeCase &t : kThemes)
            QTest::newRow(t.name) << t.id << QString::fromLatin1(t.name) << t.light;
    }

    void eachThemePaintsItsOwnTokens()
    {
        QFETCH(int, themeId);
        QFETCH(QString, name);
        QFETCH(bool, light);

        std::unique_ptr<QObject> owner(buildWindow(themeId));
        QVERIFY2(owner, "probe window failed to build");
        auto *window = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(window);
        QObject *scene = owner.get();
        window->show();

        if (auto *combo = childByName(scene, QStringLiteral("probeCombo"))) {
            if (QObject *popup = combo->property("popup").value<QObject *>())
                QMetaObject::invokeMethod(popup, "open");
        }
        QTest::qWait(120);

        const QImage shot = window->grabWindow();
        QVERIFY2(!shot.isNull(), "offscreen grabWindow returned a null image");

        const QString path = snapshotDir()
            + QStringLiteral("/theme-%1.png").arg(name);
        QVERIFY2(shot.save(path), qUtf8Printable(
            QStringLiteral("could not save %1").arg(path)));

        auto tokenColor = [&](const char *obj) {
            QQuickItem *it = childByName(scene, QString::fromLatin1(obj));
            return it ? it->property("color").value<QColor>() : QColor();
        };
        const QColor sidebarTok = tokenColor("swatchSidebar");
        const QColor surfaceTok = tokenColor("swatchSurface");
        const QColor accentTok  = tokenColor("swatchAccent");
        const QColor avatarTok  = tokenColor("avatarSwatch");

        const QColor sidebarPix = sampleAvg(shot, QRect(20, 20, 20, 20));
        const QColor surfacePix = sampleAvg(shot, QRect(80, 20, 20, 20));
        const QColor accentPix  = sampleAvg(shot, QRect(140, 20, 20, 20));

        QVERIFY2(channelDelta(sidebarPix, sidebarTok) <= 8,
            qUtf8Printable(QStringLiteral("%1: sidebar rendered %2 expected %3")
                .arg(name, sidebarPix.name(), sidebarTok.name())));
        QVERIFY2(channelDelta(surfacePix, surfaceTok) <= 8,
            qUtf8Printable(QStringLiteral("%1: surface rendered %2 expected %3")
                .arg(name, surfacePix.name(), surfaceTok.name())));
        QVERIFY2(channelDelta(accentPix, accentTok) <= 8,
            qUtf8Printable(QStringLiteral("%1: accent rendered %2 expected %3")
                .arg(name, accentPix.name(), accentTok.name())));

        const int surfaceLuma = qRound(0.299 * surfacePix.red()
            + 0.587 * surfacePix.green() + 0.114 * surfacePix.blue());
        if (light)
            QVERIFY2(surfaceLuma > 180, qUtf8Printable(
                QStringLiteral("%1: light theme surface too dark (luma %2)")
                    .arg(name).arg(surfaceLuma)));
        else
            QVERIFY2(surfaceLuma < 90, qUtf8Printable(
                QStringLiteral("%1: dark theme surface too light (luma %2)")
                    .arg(name).arg(surfaceLuma)));

        QVERIFY2(avatarTok.isValid() && avatarTok.saturation() > 40,
            qUtf8Printable(QStringLiteral("%1: avatar palette colour %2 not vivid")
                .arg(name, avatarTok.name())));

        window->hide();
    }

    void comboPopupFollowsAppliedThemePalette_data()
    {
        QTest::addColumn<int>("themeId");
        QTest::addColumn<bool>("light");
        for (const ThemeCase &t : kThemes)
            QTest::newRow(t.name) << t.id << t.light;
    }

    void comboPopupFollowsAppliedThemePalette()
    {
        QFETCH(int, themeId);
        QFETCH(bool, light);

        std::unique_ptr<QObject> owner(buildWindow(themeId));
        QVERIFY(owner);
        auto *window = qobject_cast<QQuickWindow *>(owner.get());
        QVERIFY(window);
        window->show();
        QTest::qWait(30);

        AppController controller(AppController::MockBackend);
        QVariantMap roles;
        roles.insert(QStringLiteral("window"), owner->property("tokWindow"));
        roles.insert(QStringLiteral("windowText"), owner->property("tokWindowText"));
        roles.insert(QStringLiteral("base"), owner->property("tokBase"));
        roles.insert(QStringLiteral("button"), owner->property("tokButton"));
        roles.insert(QStringLiteral("buttonText"), owner->property("tokButtonText"));
        roles.insert(QStringLiteral("highlight"), owner->property("tokHighlight"));
        roles.insert(QStringLiteral("highlightedText"), owner->property("tokHighlightedText"));
        roles.insert(QStringLiteral("text"), owner->property("tokWindowText"));
        controller.applyControlPalette(roles);
        QTest::qWait(20);

        QQuickItem *combo = childByName(owner.get(), QStringLiteral("probeCombo"));
        QVERIFY(combo);
        QObject *popup = combo->property("popup").value<QObject *>();
        QVERIFY2(popup, "ComboBox has no popup");

        const QColor windowTok = owner->property("tokWindow").value<QColor>();
        const QPalette pal = popup->property("palette").value<QPalette>();
        const QColor popupBase = pal.color(QPalette::Base);
        const QColor popupWindow = pal.color(QPalette::Window);

        auto luma = [](const QColor &c) {
            return qRound(0.299 * c.red() + 0.587 * c.green() + 0.114 * c.blue());
        };
        const int wl = luma(windowTok);
        const int bl = luma(popupBase);
        const int pwl = luma(popupWindow);
        QVERIFY2(qAbs(wl - bl) < 96 && qAbs(wl - pwl) < 96, qUtf8Printable(
            QStringLiteral("popup palette off-theme: window luma %1 base %2 window-role %3")
                .arg(wl).arg(bl).arg(pwl)));
        if (light) {
            QVERIFY2(bl > 150, "light theme popup base too dark");
        } else {
            QVERIFY2(bl < 110, "dark theme popup base too light");
        }

        window->hide();
    }
};

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    ThemeSnapshotTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "ThemeSnapshotTest.moc"
