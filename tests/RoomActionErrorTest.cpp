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
