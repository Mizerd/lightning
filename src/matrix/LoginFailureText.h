#pragma once

#include <QCoreApplication>
#include <QString>

namespace matrix::login_failure {

// The fixed reason token rust/src/lib.rs attaches to a failed password
// sign-in (HTTP status + errcode, the transport class, or a fixed word), if it
// has the shape of one. It is shown as a support code, so nothing longer or
// stranger than that vocabulary may reach the screen.
inline bool isReasonToken(const QString &reason)
{
    if (reason.isEmpty() || reason.size() > 64)
        return false;
    for (const QChar c : reason) {
        if (!(c.isLetterOrNumber() && c.unicode() < 0x80) && c != QLatin1Char('_')
            && c != QLatin1Char('.') && c != QLatin1Char('-'))
            return false;
    }
    return true;
}

// Words for a password sign-in's failure. Never the backend's message, which
// is "Matrix Rust SDK login failed: <SDK error>" and quotes the server's own
// text; the caller logs it instead. `message` is read for one distinction
// only: a client that could not be built because the server could not be
// reached or discovered.
inline QString passwordText(const QString &reason, const QString &message)
{
    // Literal per call so lupdate finds each string. The context is the one
    // these strings had in RustSdkMatrixClient.cpp, so their translations
    // still match.
    if (reason.endsWith(QLatin1String("_M_FORBIDDEN")))
        return QCoreApplication::translate("RustSdkMatrixClient",
                                           "Wrong username or password.");
    // Synapse answers an unknown user with M_FORBIDDEN; Conduit and its forks
    // say M_NOT_FOUND. A bare username is completed with the server name the
    // address resolves to, which is not always the account's own.
    if (reason.endsWith(QLatin1String("_M_NOT_FOUND")))
        return QCoreApplication::translate(
            "RustSdkMatrixClient",
            "There is no account with that username on this server. Check the "
            "username, or sign in with your full Matrix ID, like "
            "@name:example.org.");
    if (reason.contains(QLatin1String("M_LIMIT_EXCEEDED")))
        return QCoreApplication::translate(
            "RustSdkMatrixClient", "Too many attempts. Wait a minute and try again.");
    if (reason.contains(QLatin1String("M_USER_DEACTIVATED")))
        return QCoreApplication::translate("RustSdkMatrixClient",
                                           "This account has been deactivated.");
    if (reason == QLatin1String("timeout") || reason == QLatin1String("connect")
        || reason == QLatin1String("network"))
        return QCoreApplication::translate(
            "RustSdkMatrixClient",
            "Can't reach the server. Check your connection and try again.");
    if (reason == QLatin1String("client_build_failed")
        && (message.contains(QLatin1String("error sending request"))
            || message.contains(QLatin1String(".well-known"))))
        return QCoreApplication::translate(
            "RustSdkMatrixClient",
            "Can't reach a Matrix server at that address. Check the address and "
            "your connection, and try again.");
    if (isReasonToken(reason))
        return QCoreApplication::translate(
                   "RustSdkMatrixClient",
                   "Sign-in failed (%1). Check your details and the server "
                   "address, then try again.")
            .arg(reason);
    return QCoreApplication::translate(
        "RustSdkMatrixClient",
        "Sign-in failed. Check your details and the server address, then try "
        "again.");
}

} // namespace matrix::login_failure
