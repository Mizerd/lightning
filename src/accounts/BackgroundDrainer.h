#pragma once

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>

namespace lightning::accounts {

class BackgroundSessionHost;

class BackgroundDrainer : public QObject
{
    Q_OBJECT
public:
    static constexpr int kTickMs = 250;
    static constexpr qint64 kBudgetMs = 4;

    explicit BackgroundDrainer(QObject *parent = nullptr);

    void addHost(BackgroundSessionHost *host);
    void removeHost(BackgroundSessionHost *host);
    int hostCount() const { return int(m_hosts.size()); }
    bool running() const { return m_timer.isActive(); }

    int tick();

    void setElapsedForTest(std::function<qint64()> elapsedSinceTickStart);
    qint64 lastTickElapsedMsForTest() const { return m_lastTickMs; }

private:
    void updateTimer();

    QList<QPointer<BackgroundSessionHost>> m_hosts;
    int m_next = 0;
    QTimer m_timer;
    std::function<qint64()> m_elapsedForTest;
    qint64 m_lastTickMs = 0;
};

}
