#pragma once

#include <QObject>
#include <QString>

class NotificationManager : public QObject
{
    Q_OBJECT
public:
    explicit NotificationManager(QObject *parent = nullptr);

    Q_INVOKABLE void showMessage(const QString &title, const QString &body);

Q_SIGNALS:
    void activated(const QString &notificationId);
};
