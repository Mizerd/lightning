#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

#include <functional>

// "Why did I not get a notification?" A fixed list of checks, each with a
// plain verdict, run on demand, plus a real test notification.
//
// What it can and cannot know is part of the contract. It reads: whether a
// desktop notification service is on the session bus or can be started by it
// (Linux) or the tray can show a balloon (Windows, macOS), Lightning's own switch, the account's
// master mute, and the open room's mode. It CANNOT read the operating
// system's notification permission or its Focus / Do Not Disturb state on any
// platform, so that check is always "unknown" and says so; only the test
// notification settles it, because the user either sees it or does not.
class NotificationTroubleshooter : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList checks READ checks NOTIFY checksChanged)
    Q_PROPERTY(bool hasRun READ hasRun NOTIFY checksChanged)

public:
    enum Status { Pass, Warn, Fail, Unknown };
    Q_ENUM(Status)

    enum DeliveryPath { NoPath, DesktopService, TrayBalloon };
    Q_ENUM(DeliveryPath)

    struct Inputs {
        DeliveryPath delivery = NoPath;
        bool appEnabled = true;
        bool rulesKnown = false;
        bool masterMute = false;
        QString roomId;
        int roomMode = -1;
        int accountDefaultLevel = -1;
        bool trayIconShown = false;
        QString platform;
    };

    explicit NotificationTroubleshooter(QObject *parent = nullptr);

    void setInputsProvider(std::function<Inputs(const QString &roomId)> provider)
    { m_provider = std::move(provider); }
    void setTestSender(std::function<void()> sender)
    { m_testSender = std::move(sender); }

    QVariantList checks() const { return m_checks; }
    bool hasRun() const { return m_hasRun; }

    static QVariantList evaluate(const Inputs &inputs);

    static DeliveryPath probeDeliveryPath();
    static QString currentPlatform();

    Q_INVOKABLE void run(const QString &roomId);
    Q_INVOKABLE void reset();
    Q_INVOKABLE void sendTest();

Q_SIGNALS:
    void checksChanged();
    void testSent();

private:
    std::function<Inputs(const QString &)> m_provider;
    std::function<void()> m_testSender;
    QVariantList m_checks;
    bool m_hasRun = false;
};
