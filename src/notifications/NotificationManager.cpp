#include "notifications/NotificationManager.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcNotify, "matrix.notify")

NotificationManager::NotificationManager(QObject *parent)
    : QObject(parent)
{
}

void NotificationManager::showMessage(const QString &title, const QString &body)
{
    // v0.1: no tray/native path yet. Emitted-only for now.
    // v1.0: QSystemTrayIcon on Linux/Windows, NSUserNotification on macOS.
    qCInfo(lcNotify) << "notify:" << title << "-" << body;
}
