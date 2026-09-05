#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>

#include "crypto/QrImageProvider.h"

class MatrixClient;

/// MSC4108 — signing ANOTHER device in from this one.
///
/// # What this is not
///
/// It is not a way to sign THIS device in. That direction needs the OAuth
/// device-code grant, which `rust/src/oauth.rs` deliberately does not request
/// (there is a test asserting so). See rust/src/qrlogin.rs for the route if
/// that decision is ever revisited.
///
/// # Two flows, one state machine
///
/// SHOW: this device displays a QR, the new device scans it and shows two
/// digits, the user types them here.
/// ENTER: the new device displays a QR, its text is pasted here, and THIS
/// device shows two digits for the user to type over there.
///
/// Both end at a verification URL the user opens to consent, and then at the
/// new device being signed in AND cross-signed — the SDK transfers the
/// private cross-signing keys and the backup key over the channel, which is
/// why this is a security surface and not a convenience.
///
/// # Generations
///
/// Every start bumps a generation. A progress event naming an older one is
/// from a flow the user has already left; applying it would drive the current
/// flow with the previous one's input, which for a check code means asking
/// the user to compare digits from a channel that no longer exists.
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
    /// The same code as text, for a device that offers paste rather than a
    /// camera. NOT a secret in the key-material sense, but it is the channel:
    /// anyone who reads it can take the new device's place in this flow.
    Q_PROPERTY(QString qrText READ qrText NOTIFY stateChanged)
    /// The two digits WE display, in the enter flow. -1 when there are none.
    Q_PROPERTY(int checkCode READ checkCode NOTIFY stateChanged)
    /// Where the user must consent. Opened through UrlLauncher, which already
    /// refuses anything but http/https.
    Q_PROPERTY(QString verificationUri READ verificationUri NOTIFY stateChanged)
    /// A failure, in words rather than a category name.
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)

public:
    explicit QrLoginController(QObject *parent = nullptr);

    void setClient(MatrixClient *client);
    /// The store the QR grid is published into. Shared with verification:
    /// both DISPLAY one code at a time and a stale token renders nothing, so
    /// one slot is correct rather than merely convenient.
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
    /// Back to idle AND clear the code. Called on cancel, on completion and
    /// on sign-out: a grid left in the store after its flow is over is a code
    /// the next flow's URL could still serve.
    void reset();
    /// Give up the stored grid, but only if the store still holds OURS.
    void releaseStoredCode();
    void fail(const QString &category);

    MatrixClient *m_client = nullptr;
    QrCodeStore *m_store = nullptr;
    quint64 m_generation = 0;
    /// Whether a flow is RUNNING, as distinct from which one.
    ///
    /// The generation guard alone is not enough after a bare cancel with no
    /// restart: `m_generation` still names the cancelled flow, so a step for
    /// it that was already queued compares EQUAL and is applied — re-publishing
    /// the QR grid into the shared store and putting the dialog back into
    /// "showing". The contract is that the code does not outlive its flow by
    /// ANY exit, and inequality cannot express "no flow at all".
    bool m_flowActive = false;
    QString m_state = QStringLiteral("idle");
    QString m_qrToken;
    /// Whether the token currently in the shared store is OURS.
    ///
    /// The store is single-slot and shared with device verification. Clearing
    /// it unconditionally means cancelling a sign-in blanks a verification QR
    /// somebody is mid-scan of, and vice versa.
    bool m_ownsStoredCode = false;
    QString m_qrText;
    int m_checkCode = -1;
    QString m_verificationUri;
    QString m_errorText;
};
