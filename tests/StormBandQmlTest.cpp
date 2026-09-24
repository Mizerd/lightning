// StormBand.qml: the About page's procedural storm-landscape band. The runtime
// component contract (pixels are StormBandPainterTest's job): zero-warning
// load through the real "MatrixClient" QML module with the real image
// provider registered (a missing provider surfaces as a qmlWarning), fixed
// geometry, no pointer interception, and the reduced-motion / off-screen
// animation gate (the `animating` idiom from qml/Skeleton.qml). Also runs
// source-level contract scans against the file.
//
// StormBand.qml depends only on AppTheme and the Window attached property, so
// it loads directly with no AppController or context properties.
#include <QtTest/QtTest>

#include <QFile>
#include <QQmlApplicationEngine>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>

#include "app/StormBandPainter.h"

namespace {
constexpr int kSignalTimeoutMs = 3000;

QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(file.readAll());
}
} // namespace

class StormBandQmlTest : public QObject
{
    Q_OBJECT

private:
    struct Loaded {
        std::unique_ptr<QQmlApplicationEngine> engine;
        std::unique_ptr<QQuickWindow> window;
        QQuickItem *root = nullptr;
        QStringList warnings;
    };

    bool createBand(Loaded &out)
    {
        out.engine = std::make_unique<QQmlApplicationEngine>();
        // The engine owns the provider, exactly like main.cpp registering
        // "lightning-media"/"lightning-qr" before loading any QML.
        out.engine->addImageProvider(QStringLiteral("storm-band"),
                                     new StormBandImageProvider());
        connect(out.engine.get(), &QQmlEngine::warnings, this,
                [&out](const QList<QQmlError> &errors) {
                    for (const auto &e : errors)
                        out.warnings << e.toString();
                });
        QSignalSpy createdSpy(out.engine.get(),
                              &QQmlApplicationEngine::objectCreated);
        out.engine->loadFromModule(QStringLiteral("MatrixClient"),
                                   QStringLiteral("StormBand"));
        if (createdSpy.isEmpty() && !createdSpy.wait(kSignalTimeoutMs))
            return false;
        out.root = qobject_cast<QQuickItem *>(
            createdSpy.at(0).at(0).value<QObject *>());
        if (!out.root)
            return false;
        out.window = std::make_unique<QQuickWindow>();
        out.window->resize(900, 400);
        out.root->setParentItem(out.window->contentItem());
        out.root->setWidth(900);
        out.window->show();
        QCoreApplication::processEvents();
        return true;
    }

private Q_SLOTS:
    void loadsWithZeroWarningsAndFixedGeometry()
    {
        Loaded band;
        QVERIFY(createBand(band));
        QCOMPARE(band.root->implicitHeight(), 190.0);
        QVERIFY(!band.root->property("enabled").toBool());
        QCOMPARE(band.warnings, QStringList{});
    }

    void reducedMotionStopsAnimation()
    {
        Loaded band;
        QVERIFY(createBand(band));
        auto *theme = band.engine->singletonInstance<QObject *>(
            QStringLiteral("MatrixClient"), QStringLiteral("AppTheme"));
        QVERIFY(theme != nullptr);

        theme->setProperty("reducedMotion", false);
        QCoreApplication::processEvents();
        QVERIFY(band.root->property("animating").toBool());

        theme->setProperty("reducedMotion", true);
        QCoreApplication::processEvents();
        QVERIFY(!band.root->property("animating").toBool());

        theme->setProperty("reducedMotion", false);
        QCoreApplication::processEvents();
        QVERIFY(band.root->property("animating").toBool());
        QCOMPARE(band.warnings, QStringList{});
    }

    void offScreenStopsAnimation()
    {
        Loaded band;
        QVERIFY(createBand(band));
        QVERIFY(band.root->property("animating").toBool());

        band.root->setVisible(false);
        QCoreApplication::processEvents();
        QVERIFY(!band.root->property("animating").toBool());

        band.root->setVisible(true);
        QCoreApplication::processEvents();
        QVERIFY(band.root->property("animating").toBool());
    }

    void sourceContainsThePixelArtAndSafetyContract()
    {
        const QString content =
            readAll(QStringLiteral(QML_DIR "/StormBand.qml"));
        QVERIFY2(!content.isEmpty(), "StormBand.qml not readable");
        QVERIFY(content.contains(QStringLiteral("smooth: false")));
        QVERIFY(content.contains(QStringLiteral("AppTheme.reducedMotion")));
        QVERIFY(content.contains(QStringLiteral("implicitHeight: 190")));
        QVERIFY(!content.contains(QStringLiteral("WebView")));
        QVERIFY(!content.contains(QStringLiteral("WebEngineView")));
        QVERIFY(!content.contains(QStringLiteral("import QtWebEngine")));
        QVERIFY(!content.contains(QStringLiteral("MouseArea")));
        QVERIFY(!content.contains(QStringLiteral("Canvas")));
        QVERIFY(!content.contains(QStringLiteral("ShaderEffect")));
    }
};

QTEST_MAIN(StormBandQmlTest)
#include "StormBandQmlTest.moc"
