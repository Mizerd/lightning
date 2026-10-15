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

    // Everything the checks decide from; filled by the owner and by tests.
    struct Inputs {
        DeliveryPath delivery = NoPath;
        bool appEnabled = true;
        bool rulesKnown = false;
        bool masterMute = false;
        // Empty: no room is open, so the room check is left out.
        QString roomId;
        // -1 NOTHING IS STORED for this room on this device (it follows the
        // account default), 0 all messages, 1 mentions and keywords, 2 muted,
        // 3 follows the account default explicitly.
        int roomMode = -1;
        // The account's default level for this room's kind (direct message or
        // room): -1 unknown, 0 all messages, 1 mentions and keywords only. A
        // room that follows the default is only as loud as this.
        int accountDefaultLevel = -1;
        // Lightning's own tray icon exists and is visible right now. A tray
        // balloon needs it; without it the tray path delivers nothing.
        bool trayIconShown = false;
        // "linux", "windows" or "macos"; only picks the wording.
        QString platform;
    };

    explicit NotificationTroubleshooter(QObject *parent = nullptr);

    // Where the answers come from. Set once by the owner.
    void setInputsProvider(std::function<Inputs(const QString &roomId)> provider)
    { m_provider = std::move(provider); }
    void setTestSender(std::function<void()> sender)
    { m_testSender = std::move(sender); }

    QVariantList checks() const { return m_checks; }
    bool hasRun() const { return m_hasRun; }

    // Each entry: { id, title, status (Status), detail, fix } where `fix` is
    // "" or the id of an action the page offers ("enableApp", "unmute").
    static QVariantList evaluate(const Inputs &inputs);

    // What this machine can deliver through.
    static DeliveryPath probeDeliveryPath();
    static QString currentPlatform();

    Q_INVOKABLE void run(const QString &roomId);
    // Forget the last results (an account switch: they described the other
    // account).
    Q_INVOKABLE void reset();
    // Shows a real notification through the normal delivery path.
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
