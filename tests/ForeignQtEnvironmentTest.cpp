#include "app/ForeignQtEnvironment.h"

#include <QtTest/QtTest>

using namespace lightning;

class ForeignQtEnvironmentTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void recognisesQt5Locations_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("foreign");
        const char *h = "/nix/store/0123456789abcdfghijklmnpqrsvwxyz-";
        QTest::newRow("nix qtbase 5")
            << QString("%1qtbase-5.15.18-bin/lib/qt-5.15.18/plugins").arg(h) << true;
        QTest::newRow("nix qtdeclarative 5 qml")
            << QString("%1qtdeclarative-5.15.19/lib/qt-5.15.19/qml").arg(h) << true;
        QTest::newRow("nix qt5ct")
            << QString("%1qt5ct-1.9/lib/qt-6/plugins").arg(h) << true;
        QTest::newRow("debian qt5")
            << QString("/usr/lib/x86_64-linux-gnu/qt5/plugins") << true;
        QTest::newRow("qt-5 dir") << QString("/opt/x/lib/qt-5/qml") << true;
        QTest::newRow("nix qtbase 6")
            << QString("%1qtbase-6.11.1/lib/qt-6/plugins").arg(h) << false;
        QTest::newRow("qt5compat is Qt 6")
            << QString("%1qt5compat-6.8.2/lib/qt-6/qml").arg(h) << false;
        QTest::newRow("kde frameworks 6")
            << QString("%1kimageformats-6.20.0/lib/qt-6/plugins").arg(h) << false;
        QTest::newRow("debian qt6")
            << QString("/usr/lib/x86_64-linux-gnu/qt6/plugins") << false;
        QTest::newRow("no version") << QString("/home/me/plugins") << false;
        QTest::newRow("windows path")
            << QString("C:\\Qt\\qt-5.15.2\\plugins") << true;
    }
    void recognisesQt5Locations()
    {
        QFETCH(QString, path);
        QFETCH(bool, foreign);
        QCOMPARE(foreignqt::isForeignQtPath(path), foreign);
    }

    void filtersAMixedListKeepingOrder()
    {
        const QString sep(QDir::listSeparator());
        const QString in = QStringList{
            "/a/qt-6/plugins",
            "/nix/store/0123456789abcdfghijklmnpqrsvwxyz-qtbase-5.15.18/lib/qt-5.15.18/plugins",
            "/b/other",
        }.join(sep);
        QStringList dropped;
        const QByteArray out =
            foreignqt::filterPathList(in.toLocal8Bit(), &dropped);
        const QString expected =
            QStringList{ "/a/qt-6/plugins", "/b/other" }.join(sep);
        QCOMPARE(QString::fromLocal8Bit(out), expected);
        QCOMPARE(dropped.size(), 1);
    }

    void unsetsAVariableLeftWithNothingAndLeavesCleanOnesAlone()
    {
        const QString sep(QDir::listSeparator());
        qputenv("QT_PLUGIN_PATH", "/usr/lib/qt5/plugins");
        qputenv("QML2_IMPORT_PATH", "/usr/lib/qt5/qml" + sep.toLocal8Bit()
                                        + "/x/qt-6/qml");
        qputenv("QT_QPA_FONTDIR", "/fonts");
        qunsetenv("QML_IMPORT_PATH");
        qunsetenv("QT_QPA_PLATFORM_PLUGIN_PATH");
        const QStringList report = sanitizeForeignQtEnvironment();
        QCOMPARE(report.size(), 2);
        QVERIFY(!qEnvironmentVariableIsSet("QT_PLUGIN_PATH"));
        QCOMPARE(qgetenv("QML2_IMPORT_PATH"), QByteArray("/x/qt-6/qml"));
        QCOMPARE(qgetenv("QT_QPA_FONTDIR"), QByteArray("/fonts"));
        QVERIFY(sanitizeForeignQtEnvironment().isEmpty());
    }
};

QTEST_APPLESS_MAIN(ForeignQtEnvironmentTest)
#include "ForeignQtEnvironmentTest.moc"
