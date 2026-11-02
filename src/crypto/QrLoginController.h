#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include "crypto/QrImageProvider.h"

class MatrixClient;

/// MSC4108: signing another device in from this one.
///
/// Not for signing this device in; that needs the OAuth device-code grant,
/// which rust/src/oauth.rs deliberately does not request (see
/// rust/src/qrlogin.rs).
///
/// SHOW: this device displays a QR, the new device scans it and shows two
/// digits, and the user types them here.
/// ENTER: the new device displays a QR, its text is pasted here, and this
/// device shows two digits to type over there.
///
/// Both end at a verification URL for consent, then the new device is signed
/// in and cross-signed: the SDK transfers the private cross-signing keys and
/// the backup key over the channel, which makes this a security surface.
///
/// Every start bumps a generation; progress naming an older one is from an
/// abandoned flow and is dropped.
class QrLoginController : public QObject
{
    Q_OBJECT

    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString qrSource READ qrSource NOTIFY stateChanged)
    Q_PROPERTY(QString qrText READ qrText NOTIFY stateChanged)
    Q_PROPERTY(int checkCode READ checkCode NOTIFY stateChanged)
    Q_PROPERTY(QString verificationUri READ verificationUri NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)

public:
    explicit QrLoginController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    void setQrStore(QrCodeStore *store) { m_store = store; }

    bool available() const;
    QString state() const { return m_state; }
    bool busy() const;
    QString qrSource() const;
    QString qrText() const { return m_qrText; }
    int checkCode() const { return m_checkCode; }
    QString verificationUri() const { return m_verificationUri; }
    QString errorText() const { return m_errorText; }

    Q_INVOKABLE void showCode();
    Q_INVOKABLE void enterCode(const QString &payload);
    Q_INVOKABLE void submitCheckCode(int code);
    Q_INVOKABLE void cancel();

Q_SIGNALS:
    void stateChanged();

private:
    void onProgress(quint64 generation, const QString &step,
                    const QVariantMap &detail);
    void onLoggedOut();
    void reset();
    void releaseStoredCode();
    void fail(const QString &category);

    MatrixClient *m_client = nullptr;
    QrCodeStore *m_store = nullptr;
    quint64 m_generation = 0;
    bool m_flowActive = false;
    QString m_state = QStringLiteral("idle");
    QString m_qrToken;
    bool m_ownsStoredCode = false;
    QString m_qrText;
    int m_checkCode = -1;
    QString m_verificationUri;
    QString m_errorText;
};
