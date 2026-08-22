#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>

// Encoded bytes for images that are STAGED to send but not sent yet, so QML
// can show them.
//
// A clipboard paste has no file behind it — AttachmentQueueModel holds the
// encoded bytes in memory precisely so no temporary file is written — and QML
// has no way to point an Image at a QByteArray. The composer chip therefore
// showed a generic icon for every pasted image, which is the "image still has
// no preview when sent in chat" report.
//
// This is the smallest thing that fixes it without writing anything to disk:
// a token -> bytes table that StagedImageProvider reads, so a chip can use
// `image://lightning-staged/<token>`. Nothing here ever touches the filesystem
// or the network, and an entry lives exactly as long as the queued attachment
// that registered it.
//
// Security note: these are the user's OWN outgoing images, held in memory
// only, released when the attachment is removed or sent, and wiped wholesale
// on sign-out. Deliberately NOT the media cache — that one is about received
// content and has its own rules (CLAUDE.md §6).
//
// Thread affinity: registration and release happen on the GUI thread, but a
// QQuickImageProvider with ImageType::Image is called on the QML RENDER/loader
// thread when the Image is asynchronous — which every chip is. The mutex is
// what makes that safe.
class StagedImageStore : public QObject
{
    Q_OBJECT

public:
    explicit StagedImageStore(QObject *parent = nullptr);

    // Registers `bytes` and returns an opaque token, or an empty string when
    // the store is full or the bytes are empty. Tokens are never reused
    // within a session, so a stale QML binding can only miss — never resolve
    // to somebody else's image.
    QString add(const QByteArray &bytes);
    void remove(const QString &token);
    void clear();

    // Empty when the token is unknown. Safe to call from any thread.
    QByteArray bytes(const QString &token) const;

    int count() const;

    // A composer cannot queue more attachments than this in practice, and a
    // bound is what stops a leak from becoming unbounded memory.
    static constexpr int kMaxEntries = 64;

private:
    mutable QMutex m_mutex;
    QHash<QString, QByteArray> m_entries;
    quint64 m_nextToken = 1;
};
