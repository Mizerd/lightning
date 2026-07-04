#include "matrix/RustSdkMatrixClient.h"

#include "matrix_rust.h"

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcRust, "matrix.rust")

namespace {

// Small helper for the char* returned by the Rust FFI so we never forget
// mx_rust_free_cstring.
QString takeRustString(char *raw)
{
    if (!raw)
        return {};
    QString out = QString::fromUtf8(raw);
    mx_rust_free_cstring(raw);
    return out;
}

} // namespace

RustSdkMatrixClient::RustSdkMatrixClient(SettingsManager *settings, QObject *parent)
    : MatrixClient(parent)
    , m_settings(settings)
{
    qCInfo(lcRust) << "Rust SDK backend scaffold loaded:"
                   << rustBackendName()
                   << "version" << rustBackendVersion()
                   << "supports_e2ee=" << rustSupportsE2ee();
}

RustSdkMatrixClient::~RustSdkMatrixClient() = default;

QString RustSdkMatrixClient::rustBackendName() const
{
    return takeRustString(mx_rust_backend_name());
}

QString RustSdkMatrixClient::rustBackendStatus() const
{
    return takeRustString(mx_rust_status_string());
}

QString RustSdkMatrixClient::rustBackendVersion() const
{
    return takeRustString(mx_rust_version());
}

bool RustSdkMatrixClient::rustSupportsE2ee() const
{
    return mx_rust_supports_e2ee() != 0;
}

void RustSdkMatrixClient::refuseSend(const char *op)
{
    Q_EMIT errorOccurred(tr("Rust SDK backend is present but '%1' is not "
                            "implemented in the v0.4 scaffold.").arg(QLatin1String(op)));
}

void RustSdkMatrixClient::login(const QString &, const QString &, const QString &)
{
    Q_EMIT loginFailed(tr("Rust SDK backend scaffold present but login is not "
                          "wired in v0.4. Use --backend=http for a working "
                          "session."));
}

void RustSdkMatrixClient::logout()
{
    Q_EMIT loggedOut();
}

bool RustSdkMatrixClient::restoreSession() { return false; }

void RustSdkMatrixClient::startSync() {}
void RustSdkMatrixClient::stopSync()  {}

void RustSdkMatrixClient::sendTextMessage(const QString &, const QString &) { refuseSend("sendTextMessage"); }
void RustSdkMatrixClient::sendReply(const QString &, const QString &, const QString &) { refuseSend("sendReply"); }
void RustSdkMatrixClient::editMessage(const QString &, const QString &, const QString &) { refuseSend("editMessage"); }
void RustSdkMatrixClient::redactEvent(const QString &, const QString &, const QString &) { refuseSend("redactEvent"); }
void RustSdkMatrixClient::toggleReaction(const QString &, const QString &, const QString &) { refuseSend("toggleReaction"); }
void RustSdkMatrixClient::sendTyping(const QString &, bool, int) { /* silent: fires often */ }
void RustSdkMatrixClient::sendReadReceipt(const QString &, const QString &) { /* silent */ }
void RustSdkMatrixClient::sendImage(const QString &, const QString &) { refuseSend("sendImage"); }
void RustSdkMatrixClient::sendFile(const QString &, const QString &)  { refuseSend("sendFile"); }
void RustSdkMatrixClient::loadOlderMessages(const QString &) { /* no-op: no timeline yet */ }
