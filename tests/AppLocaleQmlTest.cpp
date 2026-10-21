// AppLocale through the real MatrixClient module registration (review round
// 1, L7): the QML_SINGLETON in i18n/LocalizationManager.h is what every date
// label binds to, and a registration that does not resolve fails every
// component at load. LocaleFollowsLanguageTest registers the instance by hand;
// this one goes through the module the application ships.

#include "i18n/LocalizationManager.h"

#include <QLocale>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QtTest>

#include <memory>

class AppLocaleQmlTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void theModuleSingletonIsTheProcessLocale()
    {
        const QLocale before;
        QLocale::setDefault(QLocale(QStringLiteral("en_US")));
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData(R"QML(
            import QtQml
            import MatrixClient
            QtObject {
                property var locale: AppLocale
                property string name: AppLocale.name
                property string month: Qt.locale(AppLocale.name).toString(new Date(2026, 9, 10), "MMMM")
            }
        )QML", QUrl(QStringLiteral("appLocaleProbe.qml")));
        std::unique_ptr<QObject> probe(component.create());
        QVERIFY2(probe, qPrintable(component.errorString()));
        // One instance for C++ and every engine.
        QCOMPARE(probe->property("locale").value<QObject *>(),
                 static_cast<QObject *>(AppLocale::instance()));
        QCOMPARE(probe->property("name").toString(), QLocale().bcp47Name());
        QCOMPARE(probe->property("month").toString(), QStringLiteral("October"));

        QLocale::setDefault(QLocale(QStringLiteral("de_DE")));
        AppLocale::instance()->notifyChanged();
        QCOMPARE(probe->property("name").toString(), QStringLiteral("de"));
        QCOMPARE(probe->property("month").toString(), QStringLiteral("Oktober"));
        probe.reset();
        QLocale::setDefault(before);
        AppLocale::instance()->notifyChanged();
    }
};

QTEST_MAIN(AppLocaleQmlTest)
#include "AppLocaleQmlTest.moc"
