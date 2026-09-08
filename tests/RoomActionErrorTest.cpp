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
};

QTEST_MAIN(RoomActionErrorTest)
#include "RoomActionErrorTest.moc"
