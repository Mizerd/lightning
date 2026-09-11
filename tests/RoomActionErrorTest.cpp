#include "matrix/RoomActionError.h"

#include <QFile>
#include <QtTest/QtTest>

// B021: three room-menu writes were silent when the server refused them.
//
// `rust/src/lib.rs` enqueues `room_action_error` for favourite, mark_read,
// marked_unread and read_receipt. The C++ handler swallowed all four with a
// qCWarning, so a refused Favourite left the menu looking as though it had
// worked: the list did not change, which the reader cannot tell apart from a
// slow sync.
//
// Two halves are pinned here, because either alone would be decoration. The
// mapping itself, and — since a mapping nothing calls is dead code covered by
// a passing test, which is a recorded trap in this project — a source
// contract that the production handler actually consults it and emits.
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

    // A MAPPING NOTHING CALLS IS DEAD CODE COVERED BY A PASSING TEST.
    //
    // The cases above run against the function directly, which says nothing
    // about whether the production handler reaches it. The handler lives in
    // RustSdkMatrixClient.cpp, a translation unit that needs the FFI, a tokio
    // runtime and a live client to construct, so this asserts the call site
    // in the source instead — the same shape as the other source contracts in
    // this suite family.
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

    // AN OVERFLOW MUST REPAIR THE STREAM, NOT ONLY ANNOUNCE THAT IT BROKE.
    //
    // The Rust->C++ queue drops its OLDEST entries at EVENT_QUEUE_CAP and
    // injects one `queue_overflow` marker. What it carries is POSITIONAL —
    // timeline diffs at an index, room-list index diffs — so after a drop
    // every later op addresses a vector that never received the earlier
    // ones, and only some of that is detectable: an out-of-range index is
    // caught by DiffOutcome::Invalid, but a dropped Set (a send-state
    // update, a decryption, an edit) or a dropped insert followed by
    // in-range ops passes every bounds check in silence. Nothing in the
    // payload carries a sequence number that would reveal the gap.
    //
    // The handler used to log, emit a banner and return. It must re-snapshot
    // with the two primitives this file already uses for DETECTED damage.
    // Same source-scan shape and reason as the case above: the handler needs
    // the FFI, a tokio runtime and a live client to construct.
    void anOverflowResyncsRatherThanOnlyReporting()
    {
        QFile file(QStringLiteral(SRC_DIR "/src/matrix/RustSdkMatrixClient.cpp"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
        const QString source = QString::fromUtf8(file.readAll());

        const int branch = source.indexOf(QStringLiteral("\"queue_overflow\""));
        QVERIFY2(branch > 0,
                 "the queue_overflow branch is gone; this contract is pinned "
                 "to a handler that no longer exists");
        // Bounded, and the bound is ASSERTED: an unbounded read would be
        // satisfied by the resync call that lives elsewhere in this very
        // large file, which is the opposite of what this pins.
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
