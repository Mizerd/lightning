#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>

// Encoded bytes of images staged to send, so QML can preview them. A pasted
// image has no file (AttachmentQueueModel keeps it in memory to avoid temp
// files), and QML cannot point an Image at a QByteArray, so this is a token ->
// bytes table read by StagedImageProvider (`image://lightning-staged/<token>`).
// No filesystem or network access; an entry lives as long as its attachment.
//
// These are the user's own outgoing images: memory only, released when the
// attachment is removed or sent, wiped on sign-out. Not the received-media
// cache, which has its own rules (CLAUDE.md §6).
//
// Registration happens on the GUI thread, but the provider is called on the
// QML loader thread for asynchronous Images; the mutex makes that safe.
class StagedImageStore : public QObject
{
    Q_OBJECT

public:
    explicit StagedImageStore(QObject *parent = nullptr);

    // Registers `bytes` and returns an opaque token, or "" when full or empty.
    // Tokens are never reused, so a stale binding can only miss.
    QString add(const QByteArray &bytes);
    void remove(const QString &token);
    void clear();

    // Empty when the token is unknown. Safe to call from any thread.
    QByteArray bytes(const QString &token) const;

    int count() const;

    // Bounded so a leak cannot grow memory without limit.
    static constexpr int kMaxEntries = 64;

private:
    mutable QMutex m_mutex;
    QHash<QString, QByteArray> m_entries;
    quint64 m_nextToken = 1;
};
