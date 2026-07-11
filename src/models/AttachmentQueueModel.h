#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QList>
#include <QString>
#include <QUrl>

class MatrixClient;

// v0.5.9: attachments prepared in the composer before sending.
//
// Entries come from the file picker, drag-and-drop, or clipboard image
// paste. Validation happens on add: regular readable non-empty file, MIME
// detected from *content* (QMimeDatabase), bounded against the server's
// m.upload.size when known. Nothing is uploaded until the user sends;
// dispatch itself is owned by MessageComposer. Clipboard images stay in
// memory (QByteArray) — no temporary file is ever written.
class AttachmentQueueModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

public:
    enum Roles {
        FileNameRole = Qt::UserRole + 1,
        LocalUrlRole,     // file:// URL for picked files; empty for pasted data
        MimeRole,
        SizeBytesRole,
        SizeLabelRole,
        IsImageRole,
        StateRole,        // "queued" | "dispatching" | "failed"
        ErrorRole,
    };

    struct Entry {
        QString localPath;   // empty for in-memory (clipboard) data
        QByteArray data;     // clipboard image bytes; empty for files
        QString fileName;
        QString mime;
        qint64 sizeBytes = 0;
        int width = 0;
        int height = 0;
        bool isImage = false;
        bool animated = false;
        QString state = QStringLiteral("queued");
        QString error;
        quint64 opId = 0;    // set while dispatching
    };

    explicit AttachmentQueueModel(QObject *parent = nullptr);

    void setClient(MatrixClient *client);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Add a local file (picker / drop). Returns an empty string on success
    // or a safe, user-facing reason on rejection. Never logs the path.
    Q_INVOKABLE QString addFile(const QUrl &fileUrl);
    // Add clipboard image bytes (already encoded, e.g. PNG).
    QString addImageData(const QByteArray &bytes, const QString &mime,
                         int width, int height);
    Q_INVOKABLE void removeAt(int row);
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE void retryAt(int row);

    bool isEmpty() const { return m_entries.isEmpty(); }
    QList<Entry> &entries() { return m_entries; }
    void updateEntry(int row);

    static QString humanSize(qint64 bytes);

Q_SIGNALS:
    void countChanged();

private:
    qint64 uploadLimit() const;

    MatrixClient *m_client = nullptr;
    QList<Entry> m_entries;
};
