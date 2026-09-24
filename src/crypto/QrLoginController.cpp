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
        // An account change ends the flow; the channel and its secrets belong
        // to the account that opened it.
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
        // Refused before starting: most likely an unreadable payload, so it
        // gets its own message.
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
    // The generation is only ever replaced by a new start, never zeroed.
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
    // Remove the grid too, but only if the store still holds ours: the slot is
    // shared with device verification. The store is asked (gridFor answers
    // empty for a token that is not the stored one) rather than trusting a
    // local flag.
    if (m_store && m_ownsStoredCode) {
        int modules = 0;
        if (!m_store->gridFor(token, &modules).isEmpty())
            m_store->clear();
    }
    m_ownsStoredCode = false;
}

void QrLoginController::fail(const QString &category)
{
    // Log the category (one of five fixed words), never the error text, which
    // can quote channel state and URLs. Without it a failed sign-in is
    // undiagnosable.
    qCWarning(lcQrLogin) << "qr login failed category=" << category;
    m_state = QStringLiteral("failed");
    m_flowActive = false;
    m_qrText.clear();
    m_checkCode = -1;
    releaseStoredCode();
    // Unknown categories fall back to a generic message.
    if (category == QLatin1String("expired")) {
        m_errorText = tr("The sign-in took too long and the code expired. "
                         "Start again to get a new one.");
    } else if (category == QLatin1String("cancelled")) {
        m_errorText = tr("The sign-in was cancelled on the other device.");
    } else if (category == QLatin1String("check_code")) {
        m_errorText = tr("Those digits did not match. The two devices are "
                         "not talking to each other — start again rather "
                         "than retyping.");
    } else if (category == QLatin1String("no_secrets")) {
        // The fix is one button away on the same page; name it.
        m_errorText = tr("This device has no recovery set up, so it has no "
                         "keys to send. Set up recovery and backup first, "
                         "then try again.");
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
    // A step from a flow the user left. Check both the generation (which flow)
    // and m_flowActive (whether any), since after a bare cancel the generation
    // still names the cancelled flow.
    if (generation == 0 || generation != m_generation || !m_flowActive)
        return;

    if (step == QLatin1String("qr_ready")) {
        const int modules = detail.value(QStringLiteral("qrSize")).toInt();
        const QByteArray bits = QByteArray::fromBase64(
            detail.value(QStringLiteral("qrBits")).toString().toLatin1());
        // Opaque and per-code; the URL carries nothing from the flow or
        // payload.
        const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (m_store && m_store->setCode(token, modules, bits)) {
            m_qrToken = token;
            m_ownsStoredCode = true;
        } else {
            // Geometry the renderer cannot handle; the text still works.
            qCWarning(lcQrLogin) << "sign-in code could not be rendered";
        }
        m_qrText = detail.value(QStringLiteral("qrText")).toString();
        m_state = QStringLiteral("showing");
    } else if (step == QLatin1String("check_code_needed")) {
        // The other device scanned; take our QR down so nobody else scans a
        // claimed channel.
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
        // Clear the code but keep the state; nothing is left to show.
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
