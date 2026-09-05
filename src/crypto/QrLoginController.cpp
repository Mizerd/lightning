#include "crypto/QrLoginController.h"

#include "matrix/MatrixClient.h"

#include <QLoggingCategory>
#include <QUuid>

namespace {
Q_LOGGING_CATEGORY(lcQrLogin, "matrix.qrlogin")
}

QrLoginController::QrLoginController(QObject *parent)
    : QObject(parent)
{
}

void QrLoginController::setClient(MatrixClient *client)
{
    if (m_client == client)
        return;
    if (m_client)
        m_client->disconnect(this);
    m_client = client;
    if (m_client) {
        connect(m_client, &MatrixClient::qrLoginProgress, this,
                &QrLoginController::onProgress);
        // An account change ends the flow. The channel belongs to the account
        // that opened it, and its secrets are that account's.
        connect(m_client, &MatrixClient::loggedOut, this,
                &QrLoginController::onLoggedOut);
    }
    reset();
    Q_EMIT stateChanged();
}

bool QrLoginController::available() const
{
    return m_client != nullptr && m_client->supportsQrLogin();
}

bool QrLoginController::busy() const
{
    return m_state != QLatin1String("idle") && m_state != QLatin1String("done")
        && m_state != QLatin1String("failed");
}

QString QrLoginController::qrSource() const
{
    if (m_qrToken.isEmpty())
        return {};
    return QStringLiteral("image://lightning-qr/") + m_qrToken;
}

void QrLoginController::showCode()
{
    if (!available() || busy())
        return;
    reset();
    m_generation = m_client->qrLoginGenerate();
    if (m_generation == 0) {
        fail(QStringLiteral("failed"));
        return;
    }
    m_flowActive = true;
    m_state = QStringLiteral("starting");
    Q_EMIT stateChanged();
}

void QrLoginController::enterCode(const QString &payload)
{
    if (!available() || busy() || payload.trimmed().isEmpty())
        return;
    reset();
    m_generation = m_client->qrLoginScan(payload);
    if (m_generation == 0) {
        // Refused before anything started — an unreadable payload, which is
        // by far the likeliest thing here, so it gets its own words rather
        // than the generic failure.
        m_state = QStringLiteral("failed");
        m_errorText = tr("That does not look like a sign-in code. Copy the "
                         "code from the other device exactly.");
        Q_EMIT stateChanged();
        return;
    }
    m_flowActive = true;
    m_state = QStringLiteral("starting");
    Q_EMIT stateChanged();
}

void QrLoginController::submitCheckCode(int code)
{
    if (!available() || m_generation == 0)
        return;
    if (code < 0 || code > 99) {
        m_errorText = tr("A confirmation code is two digits.");
        Q_EMIT stateChanged();
        return;
    }
    m_errorText.clear();
    m_client->qrLoginSubmitCheckCode(m_generation, code);
    m_state = QStringLiteral("waiting_for_auth");
    Q_EMIT stateChanged();
}

void QrLoginController::cancel()
{
    if (m_client)
        m_client->qrLoginCancel();
    reset();
    Q_EMIT stateChanged();
}

void QrLoginController::onLoggedOut()
{
    if (m_client)
        m_client->qrLoginCancel();
    reset();
    Q_EMIT stateChanged();
}

void QrLoginController::reset()
{
    // The generation is NOT reset to 0 here — it is only ever replaced by a
    // new start. Zeroing it would make a late progress event for the flow
    // just cancelled compare equal to "no flow" in some future caller.
    m_state = QStringLiteral("idle");
    m_flowActive = false;
    m_qrText.clear();
    m_checkCode = -1;
    m_verificationUri.clear();
    m_errorText.clear();
    releaseStoredCode();
}

void QrLoginController::releaseStoredCode()
{
    if (m_qrToken.isEmpty())
        return;
    const QString token = m_qrToken;
    m_qrToken.clear();
    // The grid goes with it: a code left in the store after its flow has
    // ended is one a stale URL could still serve.
    //
    // But ONLY if the store STILL HOLDS OURS. The slot is single and shared
    // with device verification, so clearing unconditionally would blank a
    // verification QR that had since replaced ours — mid-scan, with no
    // explanation on screen.
    //
    // The store is asked rather than a local flag consulted: a flag records
    // that we PUT a code there, which stays true after somebody else has
    // replaced it. `gridFor` answers empty for a token that is not the
    // stored one, which is exactly the question.
    if (m_store && m_ownsStoredCode) {
        int modules = 0;
        if (!m_store->gridFor(token, &modules).isEmpty())
            m_store->clear();
    }
    m_ownsStoredCode = false;
}

void QrLoginController::fail(const QString &category)
{
    m_state = QStringLiteral("failed");
    m_flowActive = false;
    m_qrText.clear();
    m_checkCode = -1;
    releaseStoredCode();
    // The bridge's categories, said in words. An unrecognised one falls
    // through to a generic message rather than showing a category name.
    if (category == QLatin1String("expired")) {
        m_errorText = tr("The sign-in took too long and the code expired. "
                         "Start again to get a new one.");
    } else if (category == QLatin1String("cancelled")) {
        m_errorText = tr("The sign-in was cancelled on the other device.");
    } else if (category == QLatin1String("check_code")) {
        m_errorText = tr("Those digits did not match. The two devices are "
                         "not talking to each other — start again rather "
                         "than retyping.");
    } else if (category == QLatin1String("unsupported")) {
        m_errorText = tr("Your homeserver does not support signing in with "
                         "a code.");
    } else {
        m_errorText = tr("The sign-in could not be completed.");
    }
    Q_EMIT stateChanged();
}

void QrLoginController::onProgress(quint64 generation, const QString &step,
                                   const QVariantMap &detail)
{
    // A step from a flow the user has already left. Applying it would drive
    // the current flow with the previous one's input — and for a check code
    // that means asking someone to compare digits from a dead channel.
    // BOTH conditions. The generation says WHICH flow; `m_flowActive` says
    // whether there is one at all — after a bare cancel the generation still
    // names the cancelled flow, so inequality alone would let its queued
    // steps through and put the code back on screen.
    if (generation == 0 || generation != m_generation || !m_flowActive)
        return;

    if (step == QLatin1String("qr_ready")) {
        const int modules = detail.value(QStringLiteral("qrSize")).toInt();
        const QByteArray bits = QByteArray::fromBase64(
            detail.value(QStringLiteral("qrBits")).toString().toLatin1());
        // Opaque and per-code, exactly as verification does it: the URL must
        // not carry anything derived from the flow or the payload.
        const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (m_store && m_store->setCode(token, modules, bits)) {
            m_qrToken = token;
            m_ownsStoredCode = true;
        } else {
            // Geometry the renderer cannot honour. The TEXT still works, and
            // is the honest thing to fall back to rather than showing an
            // unscannable picture.
            qCWarning(lcQrLogin) << "sign-in code could not be rendered";
        }
        m_qrText = detail.value(QStringLiteral("qrText")).toString();
        m_state = QStringLiteral("showing");
    } else if (step == QLatin1String("check_code_needed")) {
        // The other device scanned and is showing its digits. Our QR is no
        // longer useful and is taken down with it: leaving it up invites a
        // second device to scan a channel that is already claimed.
        releaseStoredCode();
        m_qrText.clear();
        m_state = QStringLiteral("waiting_for_code");
    } else if (step == QLatin1String("check_code_shown")) {
        m_checkCode = detail.value(QStringLiteral("checkCode"), -1).toInt();
        m_state = QStringLiteral("code_shown");
    } else if (step == QLatin1String("waiting_for_auth")) {
        m_verificationUri =
            detail.value(QStringLiteral("verificationUri")).toString();
        m_state = QStringLiteral("waiting_for_auth");
    } else if (step == QLatin1String("syncing_secrets")) {
        m_state = QStringLiteral("syncing");
    } else if (step == QLatin1String("done")) {
        // Clear the code, keep the state: the surface says it worked, and
        // there is nothing left to show or serve.
        reset();
        m_state = QStringLiteral("done");
    } else if (step == QLatin1String("failed")) {
        fail(detail.value(QStringLiteral("category")).toString());
        return;
    } else if (step == QLatin1String("starting")) {
        m_state = QStringLiteral("starting");
    }
    Q_EMIT stateChanged();
}
