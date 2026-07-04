#pragma once

#include <QObject>
#include <QString>

// v0.1: interface only. No fake crypto. Real E2EE arrives in v0.4 via the
// Matrix Rust SDK behind an FFI backend. QML uses this to render UX state
// (badges, warnings) — never to make cryptographic decisions.
class CryptoManager : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool supportsE2ee READ supportsE2ee CONSTANT)
    Q_PROPERTY(QString backendDescription READ backendDescription CONSTANT)

public:
    explicit CryptoManager(QObject *parent = nullptr);

    bool supportsE2ee() const { return false; }
    QString backendDescription() const;

    // Stubs for the future public surface. All are non-functional in v0.1.
    Q_INVOKABLE bool isDeviceVerified(const QString &userId, const QString &deviceId) const;
    Q_INVOKABLE bool isRoomEncrypted(const QString &roomId) const;
};
