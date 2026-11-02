
#include "app/GlobalShortcutsPortal.h"
#include "app/SettingsManager.h"
#include "app/ShortcutRegistry.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTest>

class GlobalShortcutsPortalNoDbusTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("MatrixClientTests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("global-shortcuts-nodbus-test"));
        QSettings settings;
        settings.clear();
    }

    void enablingWithoutDbusIsUnavailableWithAReason()
    {
        SettingsManager settings;
        ShortcutRegistry registry(&settings);
        GlobalShortcutsPortal portal(&registry);
        QSignalSpy activated(&portal, &GlobalShortcutsPortal::activated);
        QCOMPARE(portal.state(), GlobalShortcutsPortal::Off);
        portal.setEnabled(true);
        QCOMPARE(portal.state(), GlobalShortcutsPortal::Unavailable);
        QVERIFY(portal.statusText().contains(QStringLiteral("D-Bus")));
        QVERIFY(!portal.canConfigure());
        portal.configure();
        portal.setEnabled(false);
        QCOMPARE(portal.state(), GlobalShortcutsPortal::Off);
        QCOMPARE(activated.count(), 0);
    }
};

QTEST_MAIN(GlobalShortcutsPortalNoDbusTest)
#include "GlobalShortcutsPortalNoDbusTest.moc"
