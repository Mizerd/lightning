#include "matrix/RoomActionError.h"

#include <QFile>
#include <QtTest/QtTest>

// A server refusal of a room-menu write (favourite, mark_read, marked_unread,
// read_receipt: `room_action_error` from rust/src/lib.rs) must reach the
// user. Pinned: the mapping itself, and a source contract that the
// production handler consults it and emits.
class RoomActionErrorTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void everyUserInitiatedActionGetsItsOwnSentence()
    {
        const QString fav =
            matrix::room_action::userFacingError(QStringLiteral("favourite"));
        const QString read =
            matrix::room_action::userFacingError(QStringLiteral("mark_read"));
        const QString unread =
            matrix::room_action::userFacingError(QStringLiteral("marked_unread"));

        QVERIFY2(!fav.isEmpty() && !read.isEmpty() && !unread.isEmpty(),
                 "a room action the user asked for was refused and said "
                 "nothing, so the menu looks as though the write worked");
        // Distinct, because "could not favourite" and "could not mark as
        // read" are different failures and the reader should not have to
        // guess which control refused.
        QVERIFY2(fav != read && read != unread && fav != unread,
                 "two different refused actions produced the same sentence, "
                 "so the message cannot say which control failed");
    }

    void theReceiptNobodyAskedForStaysSilent()
    {
        QVERIFY2(matrix::room_action::userFacingError(
                     QStringLiteral("read_receipt")).isEmpty(),
                 "a failed read receipt was reported to the user: nobody asks "
                 "for one, the next receipt supersedes it, and a status strip "
                 "about it is noise");
    }

    void anUnknownActionSaysNothingRatherThanGuessing()
    {
        QVERIFY(matrix::room_action::userFacingError(
                    QStringLiteral("something_rust_adds_later")).isEmpty());
        QVERIFY(matrix::room_action::userFacingError(QString{}).isEmpty());
    }

    // The production handler (in RustSdkMatrixClient.cpp, which needs the FFI
    // and a live client to construct) must call the mapping, so the call site
    // is asserted in source.
    void theProductionHandlerConsultsItAndReports()
    {
        QFile file(QStringLiteral(SRC_DIR "/src/matrix/RustSdkMatrixClient.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString source = QString::fromUtf8(file.readAll());

        const int branch = source.indexOf(QStringLiteral("\"room_action_error\""));
        QVERIFY2(branch > 0,
                 "the room_action_error branch is gone; this contract is "
                 "pinned to a handler that no longer exists");
        // Bounded to the branch, so a call somewhere else in this very large
        // file cannot satisfy it.
        const QString body = source.mid(branch, 1400);
        QVERIFY2(body.contains(QStringLiteral("room_action::userFacingError")),
                 "the room_action_error branch no longer asks for a "
                 "user-facing message, so a refused Favourite or Mark as read "
                 "is silent again");
        QVERIFY2(body.contains(QStringLiteral("Q_EMIT errorOccurred")),
                 "the branch computes a message and never emits it, so "
                 "nothing reaches the status strip");
    }

    // The reason (rust/src/roomaction.rs) turns the sentence into one the
    // reader can act on, without changing which control it names.
    void theReasonSaysWhatTheReaderCanDo()
    {
        using matrix::room_action::failureHint;
        using matrix::room_action::userFacingError;
        const QString base = userFacingError(QStringLiteral("favourite"));

        const QString transport = failureHint(QStringLiteral("network"));
        QVERIFY2(!transport.isEmpty(),
                 "a dropped connection said nothing about the connection");
        QCOMPARE(failureHint(QStringLiteral("timeout")), transport);
        QCOMPARE(failureHint(QStringLiteral("connect")), transport);

        const QString busy = failureHint(QStringLiteral("http_429_M_LIMIT_EXCEEDED"));
        QVERIFY(!busy.isEmpty() && busy != transport);
        QCOMPARE(failureHint(QStringLiteral("http_502_no_errcode")), busy);

        const QString session = failureHint(QStringLiteral("refresh_failed"));
        QVERIFY(!session.isEmpty() && session != busy && session != transport);
        QCOMPARE(failureHint(QStringLiteral("http_401_M_UNKNOWN_TOKEN")), session);

        const QString refused = failureHint(QStringLiteral("http_403_M_FORBIDDEN"));
        QVERIFY(!refused.isEmpty() && refused != session);

        QCOMPARE(userFacingError(QStringLiteral("favourite"), QStringLiteral("network")),
                 base + QLatin1Char(' ') + transport);
        // A reason with nothing actionable keeps the plain sentence, never a
        // guess; and an action that is silent stays silent whatever the reason.
        QCOMPARE(userFacingError(QStringLiteral("favourite"),
                                 QStringLiteral("http_400_M_BAD_JSON")), base);
        QCOMPARE(userFacingError(QStringLiteral("favourite"), QString{}), base);
        QVERIFY(userFacingError(QStringLiteral("read_receipt"),
                                QStringLiteral("network")).isEmpty());
    }

    // "room action failed category= favourite" was the whole log line for a
    // failed Favourite: the reason was dropped in Rust. The handler must log
    // it and hand it to the sentence.
    void theProductionHandlerLogsAndUsesTheReason()
    {
        QFile file(QStringLiteral(SRC_DIR "/src/matrix/RustSdkMatrixClient.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString source = QString::fromUtf8(file.readAll());
        const int branch = source.indexOf(QStringLiteral("\"room_action_error\""));
        QVERIFY(branch > 0);
        const QString body = source.mid(branch, 1400);
        QVERIFY2(body.contains(QStringLiteral("\"reason=\"")),
                 "the failure log line does not carry the reason, so a failed "
                 "room action cannot be diagnosed from a log");
        QVERIFY2(body.contains(QStringLiteral("userFacingError(action, reason)")),
                 "the sentence ignores the reason");
        QVERIFY2(body.contains(QStringLiteral("redactId(")),
                 "the room is logged in the clear or not at all");

        QFile rust(QStringLiteral(SRC_DIR "/rust/src/lib.rs"));
        QVERIFY2(rust.open(QIODevice::ReadOnly), qPrintable(rust.fileName()));
        const QString lib = QString::fromUtf8(rust.readAll());
        // Every producer goes through the one event builder that adds the reason.
        QVERIFY2(!lib.contains(QStringLiteral("\"type\": \"room_action_error\"")),
                 "a room_action_error is still built by hand in lib.rs, without "
                 "its reason");
        QCOMPARE(lib.count(QStringLiteral("roomaction::failure_event(")), 4);
    }

    // A queue overflow must repair the stream, not only announce it. The
    // Rust->C++ queue drops its oldest entries at EVENT_QUEUE_CAP and injects
    // one `queue_overflow` marker; the payload is positional (timeline diffs,
    // room-list index diffs) and a dropped Set or insert can pass every bounds
    // check, so the handler must re-snapshot with the primitives used for
    // detected damage. Source scan, for the same reason as above.
    void anOverflowResyncsRatherThanOnlyReporting()
    {
        QFile file(QStringLiteral(SRC_DIR "/src/matrix/RustSdkMatrixClient.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString source = QString::fromUtf8(file.readAll());

        const int branch = source.indexOf(QStringLiteral("\"queue_overflow\""));
        QVERIFY2(branch > 0,
                 "the queue_overflow branch is gone; this contract is pinned "
                 "to a handler that no longer exists");
        // Bounded to the branch, so the resync call elsewhere in this large
        // file cannot satisfy it.
        const int end = source.indexOf(QStringLiteral("\nvoid RustSdkMatrixClient::"),
                                       branch);
        QVERIFY2(end > branch,
                 "the queue_overflow branch is no longer followed by another "
                 "member function — re-bound this scan before trusting it");
        const QString body = source.mid(branch, end - branch);

        QVERIFY2(body.contains(QStringLiteral("mx_rust_resync_rooms")),
                 "an overflow leaves the room-list index base unrepaired, so "
                 "a dropped index diff silently addresses the wrong room");
        QVERIFY2(body.contains(QStringLiteral("openRoomTimeline(")),
                 "an overflow leaves the open room's timeline unrepaired, so "
                 "a dropped Set or insert stays invisible until a room "
                 "switch");
    }
};

QTEST_MAIN(RoomActionErrorTest)
#include "RoomActionErrorTest.moc"
