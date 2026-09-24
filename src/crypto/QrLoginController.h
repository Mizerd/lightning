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

    /// Whether the backend can do this at all.
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    /// "idle", "starting", "showing" (our QR is up), "waiting_for_code" (the
    /// new device scanned; we need the digits), "code_shown" (we scanned;
    /// the user relays OUR digits), "waiting_for_auth", "syncing", "done",
    /// "failed".
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    /// The image URL for the QR being displayed, or empty. Served by the same
    /// provider verification uses; the token is opaque and per-code.
    Q_PROPERTY(QString qrSource READ qrSource NOTIFY stateChanged)
    /// The same code as text, for paste instead of a camera. Not key material,
    /// but it is the channel: whoever reads it can take the new device's place.
    Q_PROPERTY(QString qrText READ qrText NOTIFY stateChanged)
    /// The two digits WE display, in the enter flow. -1 when there are none.
    Q_PROPERTY(int checkCode READ checkCode NOTIFY stateChanged)
    /// Where the user consents. Opened through UrlLauncher (http/https only).
    Q_PROPERTY(QString verificationUri READ verificationUri NOTIFY stateChanged)
    /// A failure, in words rather than a category name.
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)

public:
    explicit QrLoginController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    /// The shared QR store. Verification uses it too; one slot is correct
    /// because only one code is displayed at a time.
    void setQrStore(QrCodeStore *store) { m_store = store; }

    bool available() const;
    QString state() const { return m_state; }
    bool busy() const;
    QString qrSource() const;
    QString qrText() const { return m_qrText; }
    int checkCode() const { return m_checkCode; }
    QString verificationUri() const { return m_verificationUri; }
    QString errorText() const { return m_errorText; }

    /// Start the SHOW flow — display a code here.
    Q_INVOKABLE void showCode();
    /// Start the ENTER flow from a code the other device is displaying.
    Q_INVOKABLE void enterCode(const QString &payload);
    /// Answer the SHOW flow with the digits the new device displayed.
    Q_INVOKABLE void submitCheckCode(int code);
    /// Abandon whatever is running, and forget the code.
    Q_INVOKABLE void cancel();

Q_SIGNALS:
    void stateChanged();

private:
    void onProgress(quint64 generation, const QString &step,
                    const QVariantMap &detail);
    void onLoggedOut();
    /// Back to idle and clear the code (cancel, completion, sign-out), so no
    /// stale code can be served.
    void reset();
    /// Give up the stored grid, but only if the store still holds OURS.
    void releaseStoredCode();
    void fail(const QString &category);

    MatrixClient *m_client = nullptr;
    QrCodeStore *m_store = nullptr;
    quint64 m_generation = 0;
    /// Whether any flow is running. The generation alone is not enough after a
    /// bare cancel: queued steps for the cancelled flow would still compare
    /// equal and put the code back on screen.
    bool m_flowActive = false;
    QString m_state = QStringLiteral("idle");
    QString m_qrToken;
    /// Whether the token in the shared store is ours; clearing unconditionally
    /// would blank a verification QR mid-scan.
    bool m_ownsStoredCode = false;
    QString m_qrText;
    int m_checkCode = -1;
    QString m_verificationUri;
    QString m_errorText;
};
